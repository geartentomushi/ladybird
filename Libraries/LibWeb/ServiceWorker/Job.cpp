/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/ModuleMap.h>
#include <LibWeb/HTML/Scripting/ModuleScript.h>
#include <LibWeb/HTML/Scripting/Script.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/ServiceWorker/EventNames.h>
#include <LibWeb/ServiceWorker/Job.h>
#include <LibWeb/ServiceWorker/Registration.h>
#include <LibWeb/ServiceWorker/ServiceWorker.h>
#include <LibWeb/ServiceWorker/ServiceWorkerRegistration.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>

namespace Web::ServiceWorker {

static void run_job(JS::VM&, JobQueue&);
static void finish_job(JS::VM&, GC::Ref<Job>);
static void resolve_job_promise(GC::Ref<Job>, Optional<Registration const&>, JS::Value = JS::js_null());
template<typename Error>
static void reject_job_promise(GC::Ref<Job>, Utf16String message);
static void register_(JS::VM&, GC::Ref<Job>);
static void update(JS::VM&, GC::Ref<Job>);
static void unregister(JS::VM&, GC::Ref<Job>);

GC_DEFINE_ALLOCATOR(Job);

// https://w3c.github.io/ServiceWorker/#create-job-algorithm
GC::Ref<Job> Job::create(Job::Type type, StorageAPI::StorageKey storage_key, URL::URL scope_url, URL::URL script_url, GC::Ptr<WebIDL::Promise> promise, GC::Ptr<HTML::EnvironmentSettingsObject> client)
{
    return GC::Heap::the().allocate<Job>(type, move(storage_key), move(scope_url), move(script_url), promise, client);
}

Job::Job(Job::Type type, StorageAPI::StorageKey storage_key, URL::URL scope_url, URL::URL script_url, GC::Ptr<WebIDL::Promise> promise, GC::Ptr<HTML::EnvironmentSettingsObject> client)
    : job_type(type)
    , storage_key(move(storage_key))
    , scope_url(move(scope_url))
    , script_url(move(script_url))
    , client(client)
    , job_promise(promise)
{
    // 8. If client is not null, set job’s referrer to client’s creation URL.
    if (client)
        referrer = client->creation_url;
}

Job::~Job() = default;

void Job::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(client);
    visitor.visit(job_promise);
    for (auto& job : list_of_equivalent_jobs)
        visitor.visit(job);
}

// FIXME: Does this need to be a 'user agent' level thing? Or can we have one per renderer process?
// https://w3c.github.io/ServiceWorker/#dfn-scope-to-job-queue-map
static HashMap<ByteString, NonnullOwnPtr<JobQueue>>& scope_to_job_queue_map()
{
    // NB: The jobs in here are cells of the agent that scheduled them.
    static thread_local NeverDestroyed<HashMap<ByteString, NonnullOwnPtr<JobQueue>>> map;
    return *map;
}

void forget_job_queues_of_this_agent()
{
    scope_to_job_queue_map().clear();
}

// https://w3c.github.io/ServiceWorker/#register-algorithm
static void register_(JS::VM& vm, GC::Ref<Job> job)
{
    auto script_origin = job->script_url.origin();
    auto scope_origin = job->scope_url.origin();
    auto referrer_origin = job->referrer->origin();

    // 1. If the result of running potentially trustworthy origin with the origin of job’s script url as the argument is Not Trusted, then:
    if (SecureContexts::Trustworthiness::NotTrustworthy == SecureContexts::is_origin_potentially_trustworthy(script_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise<WebIDL::SecurityError>(job, "Service Worker registration has untrustworthy script origin"_utf16);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 2. If job’s script url's origin and job’s referrer's origin are not same origin, then:
    if (!script_origin.is_same_origin(referrer_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise<WebIDL::SecurityError>(job, "Service Worker registration has incompatible script and referrer origins"_utf16);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 3. If job’s scope url's origin and job’s referrer's origin are not same origin, then:
    if (!scope_origin.is_same_origin(referrer_origin)) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise<WebIDL::SecurityError>(job, "Service Worker registration has incompatible scope and referrer origins"_utf16);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 4. Let registration be the result of running Get Registration given job’s storage key and job’s scope url.
    auto registration = Registration::get(job->storage_key, job->scope_url);

    // 5. If registration is not null, then:
    if (registration.has_value()) {
        // 1. Let newestWorker be the result of running the Get Newest Worker algorithm passing registration as the argument.
        auto* newest_worker = registration->newest_worker();

        // 2. If newestWorker is not null, job’s script url equals newestWorker’s script url,
        //    job’s worker type equals newestWorker’s type, and job’s update via cache mode's value equals registration’s update via cache mode, then:
        if (newest_worker != nullptr
            && job->script_url == newest_worker->script_url
            && job->worker_type == newest_worker->worker_type
            && job->update_via_cache == registration->update_via_cache()) {
            // 1. Invoke Resolve Job Promise with job and registration.
            resolve_job_promise(job, registration.value());

            // 2. Invoke Finish Job with job and abort these steps.
            finish_job(vm, job);
            return;
        }
    }
    // 6. Else:
    else {
        // 1. Invoke Set Registration algorithm with job’s storage key, job’s scope url, and job’s update via cache mode.
        Registration::set(job->storage_key, job->scope_url, job->update_via_cache);
    }

    // Invoke Update algorithm passing job as the argument.
    update(vm, job);
}

// Used to share internal Update algorithm state b/w fetch callbacks
class UpdateAlgorithmState : public JS::Cell {
    GC_CELL(UpdateAlgorithmState, JS::Cell);
    GC_DECLARE_ALLOCATOR(UpdateAlgorithmState);

public:
    static GC::Ref<UpdateAlgorithmState> create(Registration& registration, ServiceWorkerRecord* newest_worker)
    {
        return GC::Heap::the().allocate<UpdateAlgorithmState>(registration, newest_worker);
    }

    Registration& registration() const { return *m_registration; }
    ServiceWorkerRecord* newest_worker() const { return m_newest_worker; }

    OrderedHashMap<URL::URL, GC::Ref<Fetch::Infrastructure::ResponseView>>& updated_resource_map() { return m_map; }
    bool has_updated_resources() const { return m_has_updated_resources; }
    void set_has_updated_resources(bool b) { m_has_updated_resources = b; }

    ServiceWorkerRecord* new_worker() const { return m_new_worker.ptr(); }
    HTML::EventLoop& event_loop() const { return *m_event_loop; }
    void set_new_worker(NonnullRefPtr<ServiceWorkerRecord> worker, HTML::EventLoop& event_loop)
    {
        m_new_worker = move(worker);
        m_event_loop = event_loop;
    }

private:
    UpdateAlgorithmState(Registration& registration, ServiceWorkerRecord* newest_worker)
        : m_registration(registration)
        , m_newest_worker(newest_worker)
    {
    }

    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_map);
        visitor.visit(m_event_loop);
    }

    // NB: The registration and the newest worker are not GC-allocated. The fetch callbacks of the Update algorithm
    //     reach them through this state.
    NonnullRefPtr<Registration> m_registration;
    ServiceWorkerRecord* m_newest_worker { nullptr };
    OrderedHashMap<URL::URL, GC::Ref<Fetch::Infrastructure::ResponseView>> m_map;
    bool m_has_updated_resources { false };

    // NB: The service worker the Update algorithm creates, and the event loop of the client that runs it.
    RefPtr<ServiceWorkerRecord> m_new_worker;
    GC::Ptr<HTML::EventLoop> m_event_loop;
};

GC_DEFINE_ALLOCATOR(UpdateAlgorithmState);

// NB: The result of the processResponse steps of a job's top-level script fetch, which the perform the fetch hook
//     waits for.
struct ProcessResponseCompletionResult : RefCounted<ProcessResponseCompletionResult> {
    void visit_edges(GC::Cell::Visitor& visitor) const
    {
        if (value.has_value())
            value->visit_edges(visitor);
    }

    Optional<WebIDL::ExceptionOr<void>> value;
};

// The consume-body steps of one of a job's script fetches. They run once: when the fetch consumes its body, or when it
// is stopped.
class ScriptFetchConsumeBody : public JS::Cell {
    GC_CELL(ScriptFetchConsumeBody, JS::Cell);
    GC_DECLARE_ALLOCATOR(ScriptFetchConsumeBody);

public:
    using Steps = GC::Function<Fetch::Infrastructure::FetchAlgorithms::ProcessResponseConsumeBodyFunction::FunctionType>;

    static GC::Ref<ScriptFetchConsumeBody> create(GC::Ref<Steps> steps)
    {
        return GC::Heap::the().allocate<ScriptFetchConsumeBody>(steps);
    }

    void run(GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes)
    {
        if (auto steps = exchange(m_steps, nullptr))
            steps->function()(response, move(body_bytes));
    }

private:
    explicit ScriptFetchConsumeBody(GC::Ref<Steps> steps)
        : m_steps(steps)
    {
    }

    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_steps);
    }

    GC::Ptr<Steps> m_steps;
};

GC_DEFINE_ALLOCATOR(ScriptFetchConsumeBody);

// https://w3c.github.io/ServiceWorker/#update-registration-state-algorithm
void update_registration_state(HTML::EventLoop& event_loop, Registration& registration, RegistrationWorkerSlot target, RefPtr<ServiceWorkerRecord> source)
{
    // 1. Let registrationObjects be an array containing all the ServiceWorkerRegistration objects associated with registration.
    // 2. If target is "installing", then: set registration's installing worker to source.
    // 3. Else if target is "waiting", then: set registration's waiting worker to source.
    // 4. Else if target is "active", then: set registration's active worker to source.
    switch (target) {
    case RegistrationWorkerSlot::Installing:
        registration.set_installing_worker(source);
        break;
    case RegistrationWorkerSlot::Waiting:
        registration.set_waiting_worker(source);
        break;
    case RegistrationWorkerSlot::Active:
        registration.set_active_worker(source);
        break;
    }

    // For each registrationObject in registrationObjects, queue a task to set registrationObject's installing / waiting /
    // active attribute to null if source is null, or the result of getting the service worker object that represents
    // source in registrationObject's relevant settings object.
    event_loop.for_each_related_environment_settings_object([&](HTML::EnvironmentSettingsObject& settings) {
        auto registration_object = settings.existing_service_worker_registration_object(registration);
        if (!registration_object)
            return;
        GC::Ptr<ServiceWorker> worker_object;
        if (source)
            worker_object = settings.get_service_worker_object(source.ptr());
        HTML::queue_a_task(HTML::Task::Source::DOMManipulation, event_loop, nullptr, GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { *registration_object }, worker_object, target), [](auto& registration_object, auto& worker_object, auto& target) {
            switch (target) {
            case RegistrationWorkerSlot::Installing:
                registration_object->set_installing(worker_object);
                break;
            case RegistrationWorkerSlot::Waiting:
                registration_object->set_waiting(worker_object);
                break;
            case RegistrationWorkerSlot::Active:
                registration_object->set_active(worker_object);
                break;
            }
        }));
    });
}

// https://w3c.github.io/ServiceWorker/#update-state-algorithm
void update_worker_state(HTML::EventLoop& event_loop, ServiceWorkerRecord& worker, ServiceWorkerState state)
{
    // 1. Assert: state is not "parsed".
    VERIFY(state != ServiceWorkerState::Parsed);

    // 2. Set worker's state to state.
    worker.state = state;

    // 3. Let settingsObjects be all environment settings objects whose origin is worker's script url's origin.
    // 4. For each settingsObject of settingsObjects, queue a task on settingsObject's responsible event loop in the DOM
    //    manipulation task source to run the following steps:
    event_loop.for_each_related_environment_settings_object([&](HTML::EnvironmentSettingsObject& settings) {
        // 1. Let objectMap be settingsObject's service worker object map.
        // 2. If objectMap[worker] does not exist, then abort these steps.
        auto worker_object = settings.existing_service_worker_object(worker);
        if (!worker_object)
            return;
        HTML::queue_a_task(HTML::Task::Source::DOMManipulation, event_loop, nullptr, GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { settings }, GC::Ref { *worker_object }, state), [](auto& settings, auto& worker_object, auto& state) {
            // 3. Let workerObj be objectMap[worker].
            // 4. Set workerObj's state to state.
            worker_object->set_service_worker_state(state);

            // 5. Fire an event named statechange at workerObj.
            HTML::TemporaryExecutionContext const context(settings->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            worker_object->dispatch_event(DOM::Event::create(HTML::EventNames::statechange, HighResolutionTime::current_high_resolution_time(settings->global_object())));
        }));
    });
}

// https://w3c.github.io/ServiceWorker/#installation-algorithm (steps 8 and 9)
void fire_update_found(HTML::EventLoop& event_loop, Registration& registration)
{
    // 8. Let settingsObjects be all environment settings objects whose origin is registration's scope url's origin.
    // 9. For each settingsObject of settingsObjects, queue a task on settingsObject's responsible event loop in the DOM
    //    manipulation task source to run the following steps:
    event_loop.for_each_related_environment_settings_object([&](HTML::EnvironmentSettingsObject& settings) {
        // 1. Let registrationObjects be every ServiceWorkerRegistration object in settingsObject's realm, whose service
        //    worker registration is registration.
        auto registration_object = settings.existing_service_worker_registration_object(registration);
        if (!registration_object)
            return;

        // 2. For each registrationObject of registrationObjects, fire an event on registrationObject named updatefound.
        HTML::queue_a_task(HTML::Task::Source::DOMManipulation, event_loop, nullptr, GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { settings }, GC::Ref { *registration_object }), [](auto& settings, auto& registration_object) {
            HTML::TemporaryExecutionContext const context(settings->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            registration_object->dispatch_event(DOM::Event::create(EventNames::updatefound, HighResolutionTime::current_high_resolution_time(settings->global_object())));
        }));
    });
}

// https://w3c.github.io/ServiceWorker/#terminate-service-worker
static void terminate_service_worker(ServiceWorkerRecord& worker)
{
    if (auto agent = exchange(worker.agent, {}))
        agent->terminate();
}

// https://w3c.github.io/ServiceWorker/#run-service-worker-algorithm
static void run_service_worker(HTML::EnvironmentSettingsObject& client, Registration const& registration, ServiceWorkerRecord& worker)
{
    // NB: The worker's agent creates its ServiceWorkerGlobalScope, and fetches and evaluates its script, on its own
    //     thread. A RunScript step waits for the evaluation to complete.
    // FIXME: Run the script resource that the Update algorithm fetched instead of fetching it again.
    // FIXME: Run the service worker in an agent owned by the user agent rather than by the client.
    HTML::WorkerOptions options {};
    options.type = worker.worker_type;
    auto worker_object = client.get_service_worker_object(&worker);
    auto outside_port = HTML::MessagePort::create(client.realm());
    HTML::ServiceWorkerRegistrationSnapshot registration_snapshot {
        .scope_url = registration.scope_url(),
        .update_via_cache = registration.update_via_cache(),
    };
    worker.agent = HTML::WorkerAgentParent::create(worker.script_url, options, false, outside_port, client, worker_object, HTML::AgentType::ServiceWorker, move(registration_snapshot));
}

static void try_activate(GC::Ref<UpdateAlgorithmState>);
static void try_clear_registration(HTML::EventLoop&, Registration&);

// https://w3c.github.io/ServiceWorker/#activation-algorithm
static void activate(GC::Ref<UpdateAlgorithmState> state)
{
    auto& event_loop = state->event_loop();
    auto& registration = state->registration();

    // 1. If registration's waiting worker is null, abort these steps.
    RefPtr waiting_worker = registration.waiting_worker();
    if (!waiting_worker)
        return;

    // 2. If registration's active worker is not null, then:
    if (RefPtr active_worker = registration.active_worker()) {
        // 1. Terminate registration's active worker.
        terminate_service_worker(*active_worker);

        // 2. Run the Update Worker State algorithm passing registration's active worker and "redundant" as the arguments.
        update_worker_state(event_loop, *active_worker, ServiceWorkerState::Redundant);
    }

    // 3. Run the Update Registration State algorithm passing registration, "active" and registration's waiting worker as the arguments.
    update_registration_state(event_loop, registration, RegistrationWorkerSlot::Active, waiting_worker);

    // 4. Run the Update Registration State algorithm passing registration, "waiting" and null as the arguments.
    update_registration_state(event_loop, registration, RegistrationWorkerSlot::Waiting, nullptr);

    // 5. Run the Update Worker State algorithm passing registration's active worker and "activating" as the arguments.
    update_worker_state(event_loop, *waiting_worker, ServiceWorkerState::Activating);

    // FIXME: 6-9. Update the service worker clients that use registration, and resolve their ready promises.

    // 10. Let activeWorker be registration's active worker.
    // 11. If the result of running the Should Skip Event algorithm with activeWorker and "activate" is false, then
    //     dispatch activate at activeWorker's global object, and wait for the task to have executed.
    if (!waiting_worker->agent) {
        update_worker_state(event_loop, *waiting_worker, ServiceWorkerState::Activated);
        return;
    }
    waiting_worker->has_pending_events = true;
    waiting_worker->agent->run_service_worker_lifecycle_step(HTML::ServiceWorkerLifecycleStep::Activate, GC::create_function(GC::Heap::the(), GC::captures(state), [](auto& state, bool) {
        auto& registration = state->registration();

        // 12. Run the Update Worker State algorithm passing registration's active worker and "activated" as the arguments.
        if (auto* active_worker = registration.active_worker(); active_worker && active_worker->state == ServiceWorkerState::Activating) {
            active_worker->has_pending_events = false;
            update_worker_state(state->event_loop(), *active_worker, ServiceWorkerState::Activated);
        }

        // NB: An unregister job that ran during activation left clearing the registration to the end of the activate event.
        if (auto current = Registration::get(registration.storage_key(), registration.scope_url()); !current.has_value() || &current.value() != &registration) {
            try_clear_registration(state->event_loop(), registration);
            return;
        }

        // NB: A replacement that finished installing while this worker was activating waits for it in Try Activate.
        try_activate(state);
    }));
}

// https://w3c.github.io/ServiceWorker/#try-activate-algorithm
static void try_activate(GC::Ref<UpdateAlgorithmState> state)
{
    auto& registration = state->registration();

    // 1. If registration's waiting worker is null, return.
    if (!registration.waiting_worker())
        return;

    // 2. If registration's active worker is not null and registration's active worker's state is "activating", return.
    if (registration.active_worker() && registration.active_worker()->state == ServiceWorkerState::Activating)
        return;

    // 3. Invoke Activate with registration if either of the following is true:
    //    - registration's active worker is null.
    //    - The result of running Service Worker Has No Pending Events with registration's active worker is true, and
    //      no service worker client is using registration or registration's waiting worker's skip waiting flag is set.
    // NB: No service worker client is using any registration, since clients are not controlled yet.
    activate(state);
}

// https://w3c.github.io/ServiceWorker/#installation-algorithm
static void install(GC::Ref<Job> job, GC::Ref<UpdateAlgorithmState> state)
{
    auto& event_loop = state->event_loop();
    auto& registration = state->registration();
    NonnullRefPtr worker = *state->new_worker();

    // 1. Let installFailed be false.
    // 2. Let newestWorker be the result of running Get Newest Worker algorithm passing registration as its argument.
    // NB: The Update algorithm got the newest worker before it created worker.

    // 3. Let redundantWorker be null.
    // 4. Run the Update Registration State algorithm passing registration, "installing" and worker as the arguments.
    update_registration_state(event_loop, registration, RegistrationWorkerSlot::Installing, worker);

    // 5. Run the Update Worker State algorithm passing registration's installing worker and "installing" as the arguments.
    update_worker_state(event_loop, worker, ServiceWorkerState::Installing);

    // 6. Assert: job's job promise is not null.
    // 7. Invoke Resolve Job Promise with job and registration.
    resolve_job_promise(job, registration);

    // 8. Let settingsObjects be all environment settings objects whose origin is registration's scope url's origin.
    // 9. For each settingsObject of settingsObjects, fire updatefound at the registration's objects.
    fire_update_found(event_loop, registration);

    // 10. Let installingWorker be registration's installing worker.
    // 11. If the result of running the Should Skip Event algorithm with installingWorker and "install" is false, then
    //     dispatch install at installingWorker's global object, and set installFailed if it fails.
    // 12. Wait for task to have executed or been discarded.
    worker->agent->run_service_worker_lifecycle_step(HTML::ServiceWorkerLifecycleStep::Install, GC::create_function(GC::Heap::the(), GC::captures(job, state), [](auto& job, auto& state, bool install_succeeded) {
        auto& vm = JS::VM::the();
        auto& event_loop = state->event_loop();
        auto& registration = state->registration();
        NonnullRefPtr worker = *state->new_worker();

        // 13. If installFailed is true, then:
        if (!install_succeeded) {
            // 1. Run the Update Worker State algorithm passing registration's installing worker and "redundant" as the arguments.
            if (worker->state != ServiceWorkerState::Redundant)
                update_worker_state(event_loop, worker, ServiceWorkerState::Redundant);
            terminate_service_worker(worker);

            // 2. Run the Update Registration State algorithm passing registration, "installing" and null as the arguments.
            update_registration_state(event_loop, registration, RegistrationWorkerSlot::Installing, nullptr);

            // 3. If newestWorker is null, then remove registration map[(registration's storage key, serialized registration's scope url)].
            if (!state->newest_worker())
                Registration::remove(registration.storage_key(), registration.scope_url());

            // 4. Invoke Finish Job with job and abort these steps.
            finish_job(vm, job);
            return;
        }

        // FIXME: 14-15. Update the script resource maps of the workers.

        // 16. If registration's waiting worker is not null, then:
        if (RefPtr waiting_worker = registration.waiting_worker()) {
            // 1. Terminate registration's waiting worker.
            terminate_service_worker(*waiting_worker);

            // 2. Run the Update Worker State algorithm passing registration's waiting worker and "redundant" as the arguments.
            update_worker_state(event_loop, *waiting_worker, ServiceWorkerState::Redundant);
        }

        // 17. Run the Update Registration State algorithm passing registration, "waiting" and registration's installing worker as the arguments.
        update_registration_state(event_loop, registration, RegistrationWorkerSlot::Waiting, worker);

        // 18. Run the Update Registration State algorithm passing registration, "installing" and null as the arguments.
        update_registration_state(event_loop, registration, RegistrationWorkerSlot::Installing, nullptr);

        // 19. Run the Update Worker State algorithm passing registration's waiting worker and "installed" as the arguments.
        update_worker_state(event_loop, worker, ServiceWorkerState::Installed);

        // 20. Invoke Finish Job with job.
        finish_job(vm, job);

        // 21. Wait for all the tasks queued by Update Worker State invoked in this algorithm to have executed.
        // 22. Invoke Try Activate with registration.
        try_activate(state);
    }));
}

static GC::Ref<Fetch::Infrastructure::FetchController> fetch_job_script(Job const& job, Fetch::Infrastructure::Request& request, Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input, GC::Ref<ScriptFetchConsumeBody> consume_body)
{
    fetch_algorithms_input.process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(consume_body), [](auto& consume_body, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
        consume_body->run(response, move(body_bytes));
    });

    // FIXME: Soft-Update has no client
    auto& realm = job.client->realm();
    auto fetch_controller = Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));

    // AD-HOC: Stopping a fetch does not guarantee that its consume-body steps run, so the stop completes the script
    //         fetch as with a network error.
    fetch_controller->set_stop_steps(GC::create_function(realm.heap(), GC::captures(consume_body), [](auto& consume_body) {
        consume_body->run(Fetch::Infrastructure::ResponseView::network_error("Service Worker script fetch was stopped"_string), Empty {});
    }));
    return fetch_controller;
}

// https://w3c.github.io/ServiceWorker/#update-algorithm
static void update(JS::VM& vm, GC::Ref<Job> job)
{
    // 1. Let registration be the result of running Get Registration given job’s storage key and job’s scope url.
    auto registration = Registration::get(job->storage_key, job->scope_url);

    // 2. If registration is null, then:
    if (!registration.has_value()) {
        // 1. Invoke Reject Job Promise with job and TypeError.
        reject_job_promise<JS::TypeError>(job, "Service Worker registration not found on update"_utf16);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 3. Let newestWorker be the result of running Get Newest Worker algorithm passing registration as the argument.
    auto* newest_worker = registration->newest_worker();

    // 4. If job’s job type is update, and newestWorker is not null and its script url does not equal job’s script url, then:
    if (job->job_type == Job::Type::Update && newest_worker != nullptr && newest_worker->script_url != job->script_url) {
        // 1. Invoke Reject Job Promise with job and TypeError.
        reject_job_promise<JS::TypeError>(job, "Service Worker script URL mismatch on update"_utf16);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 5. Let hasUpdatedResources be false.
    // 6. Let updatedResourceMap be an ordered map where the keys are URLs and the values are responses.
    auto state = UpdateAlgorithmState::create(*registration, newest_worker);

    // Fetch time, with a few caveats:
    // - The spec says to use the 'to be created environment settings object for this service worker'
    // - Soft-Update has no client

    // To perform the fetch hook given request, run the following steps:
    HTML::PerformTheFetchHook perform_the_fetch_hook = GC::create_function(GC::Heap::the(), GC::captures(job, state), [](auto& job, auto& state, NonnullRefPtr<Fetch::Infrastructure::Request> request, HTML::TopLevelModule top_level, GC::Ref<ScriptFetchConsumeBody::Steps> process_custom_fetch_response) -> WebIDL::ExceptionOr<void> {
        auto& registration = state->registration();
        auto* newest_worker = state->newest_worker();

        // 1. Append `Service-Worker`/`script` to request’s header list.
        // Note: See https://w3c.github.io/ServiceWorker/#service-worker
        request->header_list()->append(HTTP::Header::isomorphic_encode("Service-Worker"sv, "script"sv));

        // 2. Set request’s cache mode to "no-cache" if any of the following are true:
        //  - registration’s update via cache mode is not "all".
        //  - job’s force bypass cache flag is set.
        //  - newestWorker is not null and registration is stale.
        if (registration.update_via_cache() != ServiceWorkerUpdateViaCache::All
            || job->force_cache_bypass
            || (newest_worker != nullptr && registration.is_stale())) {
            request->set_cache_mode(HTTP::CacheMode::NoCache);
        }

        // 3. Set request’s service-workers mode to "none".
        request->set_service_workers_mode(Fetch::Infrastructure::Request::ServiceWorkersMode::None);

        Web::Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Web::Fetch::Engine::BodyIntent::ConsumeForCallback };
        auto consume_body = ScriptFetchConsumeBody::create(process_custom_fetch_response);

        // 4. If the isTopLevel flag is unset, then return the result of fetching request.
        // FIXME: Needs spec issue, this wording is confusing and contradicts the way perform the fetch hook is used in `run a worker`
        if (top_level == HTML::TopLevelModule::No) {
            (void)fetch_job_script(*job, *request, move(fetch_algorithms_input), consume_body);
            return {};
        }

        // 5. Set request's redirect mode to "error".
        request->set_redirect_mode(Fetch::Infrastructure::Request::RedirectMode::Error);

        // 6. Fetch request, and asynchronously wait to run the remaining steps as part of fetch’s processResponse for the response response.
        // Note: The rest of the steps are in the processCustomFetchResponse algorithm
        // FIXME: Needs spec issue to mention the existence of processCustomFetchResponse, same as step 4

        // FIXME: Is there a better way to 'wait' for the fetch's processResponse to complete?
        //        Is this actually what the spec wants us to do?
        auto process_response_completion_result = make_ref_counted<ProcessResponseCompletionResult>();

        fetch_algorithms_input.process_response = GC::create_function(GC::Heap::the(), GC::captures(request, job, state, consume_body, process_response_completion_result), [](auto& request, auto& job, auto& state, auto& consume_body, auto& process_response_completion_result, GC::Ref<Fetch::Infrastructure::ResponseView> response) -> void {
            auto& registration = state->registration();
            auto* newest_worker = state->newest_worker();

            // 7. Extract a MIME type from the response’s header list. If s MIME type (ignoring parameters) is not a JavaScript MIME type, then:
            auto mime_type = Fetch::Infrastructure::extract_mime_type(response->header_list());
            if (!mime_type.has_value() || !mime_type->is_javascript()) {
                // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
                reject_job_promise<WebIDL::SecurityError>(job, "Service Worker script response is not a JavaScript MIME type"_utf16);

                // 2. Asynchronously complete these steps with a network error.
                consume_body->run(Fetch::Infrastructure::ResponseView::network_error("Service Worker script response is not a JavaScript MIME type"_string), Empty {});
                process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
                return;
            }

            // 8. Let serviceWorkerAllowed be the result of getting `Service-Worker-Allowed` from response’s header list.
            // Note: See the definition of the Service-Worker-Allowed header in Appendix B: Extended HTTP headers. https://w3c.github.io/ServiceWorker/#service-worker-allowed
            auto service_worker_allowed_bytes = response->header_list()->get("Service-Worker-Allowed"sv);

            // 9. If serviceWorkerAllowed is not null, then set serviceWorkerAllowed to the result of isomorphic decode serviceWorkerAllowed.
            Optional<Utf16String> service_worker_allowed;
            if (service_worker_allowed_bytes.has_value())
                service_worker_allowed = TextCodec::isomorphic_decode_to_utf16(*service_worker_allowed_bytes);

            // 10. Set policyContainer to the result of creating a policy container from a fetch response given response.
            // FIXME: CSP not implemented yet

            // 11. Let scopeURL be registration’s scope url.
            auto const& scope_url = registration.scope_url();

            // 12. Let maxScopeString be null.
            auto max_scope_string = Optional<ByteString> {};

            auto join_paths_with_slash = [](URL::URL const& url) -> ByteString {
                StringBuilder builder;
                builder.append('/');
                builder.join('/', url.path_segments());
                return builder.to_byte_string();
            };

            // 13. If serviceWorkerAllowed is null, then:
            if (!service_worker_allowed.has_value()) {
                // 1. Let resolvedScope be the result of parsing "./" using job’s script url as the base URL.
                auto resolved_scope = DOMURL::parse({ u"./", 2 }, job->script_url);

                // 2. Set maxScopeString to "/", followed by the strings in resolvedScope’s path (including empty strings), separated from each other by "/".
                max_scope_string = join_paths_with_slash(*resolved_scope);
            }
            // 14. Else:
            else {
                // 1. Let maxScope be the result of parsing serviceWorkerAllowed using job’s script url as the base URL.
                auto max_scope = DOMURL::parse(*service_worker_allowed, job->script_url);

                // 2. If maxScope is failure, then:
                if (!max_scope.has_value()) {
                    // 1. Asynchronously complete these steps with a network error.
                    consume_body->run(Fetch::Infrastructure::ResponseView::network_error("Failed to parse Service-Worker-Allowed header as a URL"_string), Empty {});
                    process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
                    return;
                }

                // 3. If maxScope’s origin is job’s script url's origin, then:
                if (max_scope->origin().is_same_origin(job->script_url.origin())) {
                    // 1. Set maxScopeString to "/", followed by the strings in maxScope’s path (including empty strings), separated from each other by "/".
                    max_scope_string = join_paths_with_slash(*max_scope);
                }
            }

            // 15. Let scopeString be "/", followed by the strings in scopeURL’s path (including empty strings), separated from each other by "/".
            auto scope_string = join_paths_with_slash(scope_url);

            // 16. If maxScopeString is null or scopeString does not start with maxScopeString, then:
            if (!max_scope_string.has_value() || !scope_string.starts_with(max_scope_string.value())) {
                // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
                reject_job_promise<WebIDL::SecurityError>(job, "Service Worker script scope does not match Service-Worker-Allowed header"_utf16);

                // 2. Asynchronously complete these steps with a network error.
                consume_body->run(Fetch::Infrastructure::ResponseView::network_error("Service Worker script scope does not match Service-Worker-Allowed header"_string), Empty {});
                process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
                return;
            }

            // 17. Let url be request’s url.
            auto& url = request->url();

            // 18. Set updatedResourceMap[url] to response.
            state->updated_resource_map().set(url, response);

            // 19. If response’s cache state is not "local", set registration’s last update check time to the current time.
            if (response->cache_state() != Fetch::Infrastructure::Response::CacheState::Local)
                registration.set_last_update_check_time(MonotonicTime::now());

            // 20. Set hasUpdatedResources to true if any of the following are true:
            // - newestWorker is null.
            // - newestWorker’s script url is not url or newestWorker’s type is not job’s worker type.
            // - FIXME: newestWorker’s script resource map[url]'s body is not byte-for-byte identical with response’s body.
            if (newest_worker == nullptr
                || newest_worker->script_url != url
                || newest_worker->worker_type != job->worker_type) {
                state->set_has_updated_resources(true);
            }

            // 21. If hasUpdatedResources is false and newestWorker’s classic scripts imported flag is set, then:
            if (!state->has_updated_resources()) {
                VERIFY(newest_worker);
                if (newest_worker->classic_scripts_imported) {
                    // 1. For each importUrl → storedResponse of newestWorker’s script resource map:
                    if (false) {
                        // FIXME: 1. If importUrl is url, then continue.
                        // FIXME: 2. Let importRequest be a new request whose url is importUrl, client is job’s client, destination is "script", parser metadata
                        //    is "not parser-inserted", and whose use-URL-credentials flag is set.
                        // FIXME: 3. Set importRequest’s cache mode to "no-cache" if any of the following are true:
                        //     * registration’s update via cache mode is "none".
                        //     * job’s force bypass cache flag is set.
                        //     * registration is stale.
                        // FIXME: 4. Let fetchedResponse be the result of fetching importRequest.
                        // FIXME: 5. Set updatedResourceMap[importRequest’s url] to fetchedResponse.
                        // FIXME: 6. Set fetchedResponse to fetchedResponse’s unsafe response.
                        // FIXME: 7. If fetchedResponse’s cache state is not "local", set registration’s last update check time to the current time.
                        // FIXME: 8. If fetchedResponse is a bad import script response, continue.
                        // NOTE: Bad responses for importScripts() are ignored for the purpose of the byte-to-byte check. Only good responses for the incumbent worker
                        //       and good responses for the potential update worker are considered. See issue #1374 for some rationale
                        // FIXME: 9. If fetchedResponse’s body is not byte-for-byte identical with storedResponse’s unsafe response’s body, set hasUpdatedResources to true.
                        // NOTE: The control does not break the loop in this step to continue with all the imported scripts to populate the cache.
                    }
                }
            }

            // 22. Asynchronously complete these steps with response.
            process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
        });

        auto fetch_controller = fetch_job_script(*job, *request, move(fetch_algorithms_input), consume_body);

        // FIXME: This feels.. uncomfortable but it should work to block the current task until the fetch has progressed past our processResponse hook or aborted
        auto& event_loop = job->client ? job->client->responsible_event_loop() : HTML::current_event_loop();
        event_loop.spin_until(GC::create_function(GC::Heap::the(), GC::captures(fetch_controller, process_response_completion_result), [](auto& fetch_controller, auto& process_response_completion_result) -> bool {
            if (process_response_completion_result->value.has_value())
                return true;
            if (fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Terminated || fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Aborted) {
                process_response_completion_result->value = WebIDL::AbortError::create("Service Worker fetch was terminated or aborted"_utf16);
                return true;
            }
            // NB: A stopped fetch's stop steps have completed the script fetch.
            if (fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Stopped) {
                process_response_completion_result->value = WebIDL::ExceptionOr<void> {};
                return true;
            }
            return false;
        }));

        // NB: The event loop gave up on an aborted script.
        if (!process_response_completion_result->value.has_value())
            return WebIDL::AbortError::create("Service Worker fetch was terminated or aborted"_utf16);
        return process_response_completion_result->value.release_value();
    });

    // When the algorithm asynchronously completes, continue the rest of these steps, with script being the asynchronous completion value.
    HTML::OnFetchScriptComplete on_fetch_complete = GC::create_function(GC::Heap::the(), GC::captures(job, state), [](auto& job, auto& state, GC::Ptr<HTML::Script> script) -> void {
        auto& vm = JS::VM::the();
        auto& registration = state->registration();
        auto* newest_worker = state->newest_worker();

        // 8. If script is null or Is Async Module with script’s record, script’s base URL, and « » is true, then:
        // FIXME: Reject async modules
        if (!script) {
            // 1. Invoke Reject Job Promise with job and TypeError.
            reject_job_promise<JS::TypeError>(job, "Service Worker script is not a valid module"_utf16);

            // 2. If newestWorker is null, then remove registration map[(registration’s storage key, serialized scopeURL)].
            if (newest_worker == nullptr)
                Registration::remove(registration.storage_key(), registration.scope_url());

            // 3. Invoke Finish Job with job and abort these steps.
            finish_job(vm, job);
            return;
        }

        // 9. If hasUpdatedResources is false, then:
        if (!state->has_updated_resources()) {
            // 1. Set registration’s update via cache mode to job’s update via cache mode.
            registration.set_update_via_cache(job->update_via_cache);

            // 2. Invoke Resolve Job Promise with job and registration.
            resolve_job_promise(job, registration);

            // 3. Invoke Finish Job with job and abort these steps.
            finish_job(vm, job);
            return;
        }

        // 10. Let worker be a new service worker.
        auto worker = adopt_ref(*new ServiceWorkerRecord);

        // 11. Set worker’s script url to job’s script url, worker’s script resource to script, worker’s type to job’s worker type, and worker’s script resource map to updatedResourceMap.
        worker->script_url = job->script_url;
        worker->worker_type = job->worker_type;

        // FIXME: 12. Append url to worker’s set of used scripts.
        // FIXME: 13. Set worker’s script resource’s policy container to policyContainer.
        // FIXME: 14. Let forceBypassCache be true if job’s force bypass cache flag is set, and false otherwise.

        // NB: The worker's agent is started by job's client.
        if (!job->client) {
            if (newest_worker == nullptr)
                Registration::remove(registration.storage_key(), registration.scope_url());
            finish_job(vm, job);
            return;
        }
        state->set_new_worker(worker, job->client->responsible_event_loop());

        // 15. Let runResult be the result of running the Run Service Worker algorithm with worker and forceBypassCache.
        run_service_worker(*job->client, registration, worker);
        worker->agent->run_service_worker_lifecycle_step(HTML::ServiceWorkerLifecycleStep::RunScript, GC::create_function(GC::Heap::the(), GC::captures(job, state), [](auto& job, auto& state, bool run_succeeded) {
            auto& vm = JS::VM::the();
            auto& registration = state->registration();

            // 16. If runResult is failure or an abrupt completion, then:
            if (!run_succeeded) {
                terminate_service_worker(*state->new_worker());

                // 1. Invoke Reject Job Promise with job and TypeError.
                reject_job_promise<JS::TypeError>(job, "Service Worker script failed to run"_utf16);

                // 2. If newestWorker is null, then remove registration map[(registration’s storage key, serialized scopeURL)].
                if (state->newest_worker() == nullptr)
                    Registration::remove(registration.storage_key(), registration.scope_url());

                // 3. Invoke Finish Job with job.
                finish_job(vm, job);
                return;
            }

            // 17. Else, invoke Install algorithm with job, worker, and registration as its arguments.
            install(job, state);
        }));
    });

    // 7. Switching on job’s worker type, run these substeps with the following options:
    switch (job->worker_type) {
    case WorkerType::Classic:
        // 1. Fetch a classic worker script given job’s serialized script url, job’s client, "serviceworker", and the to-be-created environment settings object for this service worker.
        // FIXME: Credentials mode
        // FIXME: Use a 'stub' service worker ESO as the fetch "environment"
        (void)HTML::fetch_classic_worker_script(job->script_url, GC::Ref { *job->client }, Fetch::Infrastructure::Request::Destination::ServiceWorker, *job->client, perform_the_fetch_hook, on_fetch_complete);
        break;
    case WorkerType::Module:
        // 2. Fetch a module worker script graph given job’s serialized script url, job’s client, "serviceworker", "omit", and the to-be-created environment settings object for this service worker.
        // FIXME: Credentials mode
        // FIXME: Use a 'stub' service worker ESO as the fetch "environment"
        (void)HTML::fetch_module_worker_script_graph(job->script_url, GC::Ref { *job->client }, Fetch::Infrastructure::Request::Destination::ServiceWorker, *job->client, perform_the_fetch_hook, on_fetch_complete);
        break;
    }
}

// https://w3c.github.io/ServiceWorker/#clear-registration-algorithm
static void clear_registration(HTML::EventLoop& event_loop, Registration& registration)
{
    // 1. Run the following steps atomically.
    // 2. If registration's installing worker is not null, then:
    if (RefPtr installing_worker = registration.installing_worker()) {
        // 1. Terminate registration's installing worker.
        terminate_service_worker(*installing_worker);

        // 2. Run the Update Worker State algorithm passing registration's installing worker and "redundant" as the arguments.
        update_worker_state(event_loop, *installing_worker, ServiceWorkerState::Redundant);

        // 3. Run the Update Registration State algorithm passing registration, "installing" and null as the arguments.
        update_registration_state(event_loop, registration, RegistrationWorkerSlot::Installing, nullptr);
    }

    // 3. If registration's waiting worker is not null, then:
    if (RefPtr waiting_worker = registration.waiting_worker()) {
        // 1. Terminate registration's waiting worker.
        terminate_service_worker(*waiting_worker);

        // 2. Run the Update Worker State algorithm passing registration's waiting worker and "redundant" as the arguments.
        update_worker_state(event_loop, *waiting_worker, ServiceWorkerState::Redundant);

        // 3. Run the Update Registration State algorithm passing registration, "waiting" and null as the arguments.
        update_registration_state(event_loop, registration, RegistrationWorkerSlot::Waiting, nullptr);
    }

    // 4. If registration's active worker is not null, then:
    if (RefPtr active_worker = registration.active_worker()) {
        // 1. Terminate registration's active worker.
        terminate_service_worker(*active_worker);

        // 2. Run the Update Worker State algorithm passing registration's active worker and "redundant" as the arguments.
        update_worker_state(event_loop, *active_worker, ServiceWorkerState::Redundant);

        // 3. Run the Update Registration State algorithm passing registration, "active" and null as the arguments.
        update_registration_state(event_loop, registration, RegistrationWorkerSlot::Active, nullptr);
    }
}

// https://w3c.github.io/ServiceWorker/#try-clear-registration-algorithm
static void try_clear_registration(HTML::EventLoop& event_loop, Registration& registration)
{
    // 1. Invoke Clear Registration with registration if no service worker client is using registration and all of the
    //    following conditions are true:
    //    - registration's installing worker is null or the result of running Service Worker Has No Pending Events
    //      with registration's installing worker is true.
    //    - registration's waiting worker is null or the result of running Service Worker Has No Pending Events with
    //      registration's waiting worker is true.
    //    - registration's active worker is null or the result of running Service Worker Has No Pending Events with
    //      registration's active worker is true.
    // NB: No service worker client is using any registration, since clients are not controlled yet. Unregister jobs
    //     wait in the job queue until the registration's install has finished, so only an activate event can be
    //     pending here.
    // FIXME: Also count functional events once workers handle them.
    for (auto* worker : { registration.installing_worker(), registration.waiting_worker(), registration.active_worker() }) {
        if (worker && worker->has_pending_events)
            return;
    }
    clear_registration(event_loop, registration);
}

// https://w3c.github.io/ServiceWorker/#unregister-algorithm
static void unregister(JS::VM& vm, GC::Ref<Job> job)
{
    // NB: Without a client there is no promise to settle and no event loop to update.
    if (!job->client) {
        finish_job(vm, job);
        return;
    }

    // 1. If the origin of job's scope url is not job's client's origin, then:
    if (!job->scope_url.origin().is_same_origin(job->client->origin())) {
        // 1. Invoke Reject Job Promise with job and "SecurityError" DOMException.
        reject_job_promise<WebIDL::SecurityError>(job, "Service Worker unregistration has a cross-origin scope"_utf16);

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }

    // 2. Let registration be the result of running Get Registration given job's storage key and job's scope url.
    auto maybe_registration = Registration::get(job->storage_key, job->scope_url);

    // 3. If registration is null, then:
    if (!maybe_registration.has_value()) {
        // 1. Invoke Resolve Job Promise with job and false.
        resolve_job_promise(job, {}, JS::Value(false));

        // 2. Invoke Finish Job with job and abort these steps.
        finish_job(vm, job);
        return;
    }
    NonnullRefPtr registration = maybe_registration.value();

    // 4. Remove registration map[(registration's storage key, job's scope url)].
    Registration::remove(registration->storage_key(), job->scope_url);

    // 5. Invoke Resolve Job Promise with job and true.
    resolve_job_promise(job, {}, JS::Value(true));

    // 6. Invoke Try Clear Registration with registration.
    try_clear_registration(job->client->responsible_event_loop(), registration);

    // 7. Invoke Finish Job with job.
    finish_job(vm, job);
}

// https://w3c.github.io/ServiceWorker/#run-job-algorithm
static void run_job(JS::VM&, JobQueue& job_queue)
{
    // 1. Assert: jobQueue is not empty.
    VERIFY(!job_queue.is_empty());

    // 2. Queue a task to run these steps:
    // NB: The job queue is not GC-allocated, so the task reaches it through the job at its front, which is not dequeued
    //     before the task runs.
    auto job_run_steps = GC::create_function(GC::Heap::the(), GC::captures(job_queue.first()), [](auto& first_job) {
        auto& vm = JS::VM::the();
        auto& job_queue = *first_job->containing_job_queue;

        // 1. Let job be the first item in jobQueue.
        auto& job = job_queue.first();

        // FIXME: Do these really need to be in parallel to the HTML event loop? Sounds fishy
        switch (job->job_type) {
        case Job::Type::Register:
            // 2. If job’s job type is register, run Register with job in parallel.
            register_(vm, job);
            break;
        case Job::Type::Update:
            // 3. If job’s job type is update, run Update with job in parallel.
            update(vm, job);
            break;
        case Job::Type::Unregister:
            // 4. If job’s job type is unregister, run Unregister with job in parallel.
            unregister(vm, job);
            break;
        }
    });

    // FIXME: How does the user agent ensure this happens? Is this a normative note?
    // NOTE: For a register job and an update job, the user agent delays queuing a task for running the job until after
    //       a DOMContentLoaded event has been dispatched to the document that initiated the job.

    // FIXME: Spec should be updated to avoid 'queue a task' and use 'queue a global task' instead
    // FIXME: On which task source? On which event loop? On behalf of which document?
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, job_run_steps);
}

// https://w3c.github.io/ServiceWorker/#finish-job-algorithm
static void finish_job(JS::VM& vm, GC::Ref<Job> job)
{
    // 1. Let jobQueue be job’s containing job queue.
    auto& job_queue = *job->containing_job_queue;

    // 2. Assert: the first item in jobQueue is job.
    VERIFY(job_queue.first() == job);

    // 3. Dequeue from jobQueue
    (void)job_queue.take_first();

    // 4. If jobQueue is not empty, invoke Run Job with jobQueue.
    if (!job_queue.is_empty())
        run_job(vm, job_queue);
}

// https://w3c.github.io/ServiceWorker/#resolve-job-promise-algorithm
static void resolve_job_promise_of(GC::Ref<Job> job, Optional<Registration const&> registration, JS::Value value)
{
    auto& realm = job->client->realm();

    // 1. Let convertedValue be null.
    // 2. If job’s job type is either register or update, set convertedValue to the result of getting the service
    //    worker registration object that represents value in job’s client.
    // NB: The registration object is created now, so that it reflects registration's workers at this point.
    GC::Ptr<ServiceWorkerRegistration> registration_object;
    if (registration.has_value() && first_is_one_of(job->job_type, Job::Type::Register, Job::Type::Update))
        registration_object = job->client->get_service_worker_registration_object(registration.value());

    HTML::queue_a_task(HTML::Task::Source::DOMManipulation, job->client->responsible_event_loop(), nullptr, GC::create_function(GC::Heap::the(), GC::captures(realm, job, value, registration_object), [](auto& realm, auto& job, auto& value, auto& registration_object) {
        HTML::TemporaryExecutionContext const context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 3. Else, set convertedValue to value, in job’s client's Realm.
        JS::Value converted_value = value;
        if (registration_object)
            converted_value = Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { *registration_object });

        // 4. Resolve job’s job promise with convertedValue.
        WebIDL::resolve_promise(*job->job_promise, converted_value);
    }));
}

static void resolve_job_promise(GC::Ref<Job> job, Optional<Registration const&> registration, JS::Value value)
{
    // 1. If job’s client is not null, queue a task, on job’s client's responsible event loop using the DOM
    //    manipulation task source, to resolve job’s job promise.
    if (job->client)
        resolve_job_promise_of(job, registration, value);

    // 2. For each equivalentJob in job’s list of equivalent jobs:
    for (auto& equivalent_job : job->list_of_equivalent_jobs) {
        // 1. If equivalentJob’s client is null, continue.
        if (!equivalent_job->client)
            continue;

        // 2. Queue a task, on equivalentJob’s client's responsible event loop using the DOM manipulation task source,
        //    to resolve equivalentJob’s job promise.
        resolve_job_promise_of(equivalent_job, registration, value);
    }
}

// https://w3c.github.io/ServiceWorker/#reject-job-promise-algorithm
template<typename Error>
static void reject_job_promise(GC::Ref<Job> job, Utf16String message)
{
    // 1. If job’s client is not null, queue a task, on job’s client's responsible event loop using the DOM manipulation task source,
    //    to reject job’s job promise with a new exception with errorData and a user agent-defined message, in job’s client's Realm.
    if (job->client) {
        auto& realm = job->client->realm();
        HTML::queue_a_task(HTML::Task::Source::DOMManipulation, job->client->responsible_event_loop(), nullptr, GC::create_function(GC::Heap::the(), GC::captures(realm, job, message), [](auto& realm, auto& job, auto& message) {
            HTML::TemporaryExecutionContext const context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            WebIDL::reject_promise(*job->job_promise, Error::create(realm, message));
        }));
    }

    // 2. For each equivalentJob in job’s list of equivalent jobs:
    for (auto& equivalent_job : job->list_of_equivalent_jobs) {
        // 1. If equivalentJob’s client is null, continue.
        if (!equivalent_job->client)
            continue;

        // 2. Queue a task, on equivalentJob’s client's responsible event loop using the DOM manipulation task source,
        //    to reject equivalentJob’s job promise with a new exception with errorData and a user agent-defined message,
        //    in equivalentJob’s client's Realm.
        auto& realm = equivalent_job->client->realm();
        HTML::queue_a_task(HTML::Task::Source::DOMManipulation, equivalent_job->client->responsible_event_loop(), nullptr, GC::create_function(GC::Heap::the(), GC::captures(realm, equivalent_job, message), [](auto& realm, auto& equivalent_job, auto& message) {
            HTML::TemporaryExecutionContext const context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            WebIDL::reject_promise(*equivalent_job->job_promise, Error::create(realm, message));
        }));
    }
}

// https://w3c.github.io/ServiceWorker/#schedule-job-algorithm
void schedule_job(JS::VM& vm, GC::Ref<Job> job)
{
    // 1. Let jobQueue be null.
    // Note: See below for how we ensure job queue

    // 2. Let jobScope be job’s scope url, serialized.
    // FIXME: Suspect that spec should specify to not use fragment here
    auto job_scope = job->scope_url.serialize().to_byte_string();

    // 3. If scope to job queue map[jobScope] does not exist, set scope to job queue map[jobScope] to a new job queue.
    // 4. Set jobQueue to scope to job queue map[jobScope].
    auto& job_queue = *scope_to_job_queue_map().ensure(job_scope, [] {
        return make<JobQueue>();
    });

    // 5. If jobQueue is empty, then:
    if (job_queue.is_empty()) {
        // 2. Set job’s containing job queue to jobQueue, and enqueue job to jobQueue.
        job->containing_job_queue = &job_queue;
        job_queue.append(job);
        run_job(vm, job_queue);
    }
    // 6. Else:
    else {
        // 1. Let lastJob be the element at the back of jobQueue.
        auto& last_job = job_queue.last();

        // 2. If job is equivalent to lastJob and lastJob’s job promise has not settled, append job to lastJob’s list of equivalent jobs.
        // FIXME: There's no WebIDL AO that corresponds to checking if an ECMAScript promise has settled
        if (job == last_job && !as<JS::Promise>(*job->job_promise->promise()).is_handled()) {
            last_job->list_of_equivalent_jobs.append(job);
        }
        // 3. Else, set job’s containing job queue to jobQueue, and enqueue job to jobQueue.
        else {
            job->containing_job_queue = &job_queue;
            job_queue.append(job);
        }
    }
}

}

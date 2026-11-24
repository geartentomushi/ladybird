
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("../resources/testharness.js");
self.META_TITLE = "tests for navigator.userAgentData";
importScripts("useragentdata.https.any.js");
done();

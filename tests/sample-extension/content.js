/* Content script: proves the isolated world, the shim, storage and messaging. */
(function () {
    var banner = document.createElement("div");
    banner.id = "atlantic-ext-smoke";
    banner.textContent = "extension loading…";
    document.documentElement.appendChild(banner);

    function report(text) {
        banner.textContent = text;
    }

    // 1. i18n + getURL, entirely local to the shim.
    var label = browser.i18n.getMessage("badgeLabel", [location.hostname]);

    // 2. storage round-trip through the UI process.
    browser.storage.local.get({ visits: 0 }).then(function (stored) {
        var visits = stored.visits + 1;
        return browser.storage.local.set({ visits: visits }).then(function () {
            return visits;
        });
    }).then(function (visits) {
        // 3. message the background context and wait for its answer.
        return browser.runtime.sendMessage({ type: "seen", host: location.hostname })
            .then(function (response) {
                report(label + " · visit #" + visits + " · bg says: " + (response && response.ack));
            });
    }).catch(function (error) {
        report("FAILED: " + error.message);
    });

    // 4. the background can call back into this tab.
    browser.runtime.onMessage.addListener(function (message) {
        if (message && message.type === "ping")
            return { pong: location.hostname };
    });
})();

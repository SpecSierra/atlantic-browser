/* Background context: proves the JSC host, timers, tabs and messaging. */
var seen = 0;

browser.runtime.onMessage.addListener(function (message, sender) {
    if (!message || message.type !== "seen")
        return;
    seen++;
    console.log("smoke: " + message.host + " (tab " + (sender.tab && sender.tab.id) + ")");
    browser.action.setBadgeText({ text: String(seen) });
    return { ack: seen };
});

browser.runtime.onStartup.addListener(function () {
    console.log("smoke: background started");
});

// Timers come from the preamble, not from a DOM window.
setTimeout(function () {
    browser.tabs.query({ active: true }).then(function (tabs) {
        console.log("smoke: active tab is " + (tabs[0] && tabs[0].url));
    });
}, 2000);

browser.tabs.onUpdated.addListener(function (tabId, changeInfo) {
    if (changeInfo.status === "complete")
        console.log("smoke: tab " + tabId + " finished " + changeInfo.url);
});

/* Extension page: proves the shim runs in the page's own world. */
function show(id, text, bad) {
    var node = document.getElementById(id);
    node.textContent = text;
    if (bad) node.className = "bad";
}

show("id", browser.runtime.id);

browser.storage.local.get({ visits: 0 })
    .then(function (stored) { show("visits", String(stored.visits)); })
    .catch(function (e) { show("visits", e.message, true); });

browser.tabs.query({ active: true })
    .then(function (tabs) { show("tab", tabs.length ? tabs[0].url : "(none)"); })
    .catch(function (e) { show("tab", e.message, true); });

document.getElementById("reset").addEventListener("click", function () {
    browser.storage.local.set({ visits: 0 }).then(function () { show("visits", "0"); });
});

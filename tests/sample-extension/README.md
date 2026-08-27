# Extension smoke test

A minimal WebExtension that exercises everything Atlantic implements. Copy it to
the device and load it, then check each of the five things it proves:

```
scp -P 2222 -r tests/sample-extension \
    root@localhost:.local/share/org.sailfishos/browser/extensions/smoke-test
```

Restart the browser (or Settings → Extensions → Reload extensions).

| What you should see | Proves |
|---|---|
| A blue banner bottom-left on every page, reading `Atlantic sees <host> · visit #N · bg says: N` | content script + CSS in the isolated world, `i18n`, `storage.local`, `runtime.sendMessage` round trip |
| The visit count rising across page loads and surviving a browser restart | storage persistence |
| `smoke: <host> (tab N)` in the browser log | the JSC background context and the sender info |
| `smoke: active tab is …` about two seconds after start | preamble timers and `browser.tabs` |
| Settings → Extensions → ⋯ → Open popup showing an id, a count and the active tab URL | `atlantic-extension://` pages and the main-world shim |

A banner reading `FAILED: …` names the first API that broke.

# Replacement NGS server

Gun Bros talked to Glu's own backend ("NGS") at `http://ng-gunbros-android.glu.com/gbro/` for
server time, Glu accounts, friends ("brothers"), messages, leaderboards and more. That server is
gone. `ngs_server.py` is a small stand-in that answers the requests reverse-engineered so far and
logs everything else, so the API can be mapped one request at a time.

## Running it against a phone over USB

```bash
python3 server/ngs_server.py --port 18080
adb reverse tcp:18080 tcp:18080
adb shell setprop debug.gunbros.ngs_host 127.0.0.1
adb shell setprop debug.gunbros.ngs_port 18080
# restart the game
```

With `debug.gunbros.ngs_host` set, the bridge resolves `*.glu.com` to that host instead of blocking
it, and creates a Glu account automatically on first launch (the Android port never shows the
game's register dialog). Unset it (`adb shell setprop debug.gunbros.ngs_host ""`) to go back to
offline behaviour.

Runtime files, next to the script: `accounts.json` (accounts; passwords stored as salted SHA-256)
and `capture.jsonl` (every request, passwords redacted).

## Protocol notes

- JSON over HTTP `PUT`, both directions:
  `{"payload": [{"version": 1, "sequenceId": N, "message": {...}}]}`.
  Replies are matched to requests by `sequenceId`.
- Request messages carry `header` = `{device, game{id: 622111515, ...}, sessionid, mac, type}`.
- A reply message with `errorcode` is a failure; the game then reads `list` as the error text.
  Any other reply must also contain `list`: `CNGSServerObject::WasErrorInResponse` dereferences it
  without a null check, and a missing `list` makes the game jump to address 0.
- Glu wide strings in the binary are UTF-16.

| Endpoint | Status |
|---|---|
| `resources/user/getnettime2` | `list` = server time in ms |
| `resources/user/registeruser/` | creates the account |
| `resources/user/validateuser/` | logs in (not yet reached by the game) |
| `resources/user/attributes/` | stores user attributes |
| `resources/friend/search` | friends list, currently always empty |
| everything else | logged, answered 404 |

Open problem: the game doesn't receive its account id or a session after registering
(`Credentials.dat` keeps `clientid: -1`), so later requests can't yet be tied to a player.

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
| `resources/user/validateuser/` | logs in; the device-level login (by `GLUSOCIALID`) returns the session and credentials, giving the game its client id |
| `resources/user/attributes/` | stores user attributes |
| `resources/friend/search` | the player's friends ("brothers") as credentials objects |
| `resources/message/poll` | inbox, currently always empty |
| `resources/avatar/avatarattributes` | stores the player's profile data (only their own) |
| `resources/avatar/queryattributes` | returns a player's stored profile data, e.g. friends' level and loadout for Brotherhood |
| everything else | logged, answered 404 |

## Login flow

1. On first launch the bridge registers a Glu account (`registeruser`), as described above.
2. Bridge patch P makes the game run its device-level login (`validateuser` with
   `userattribute: GLUSOCIALID`) after that, which the original game did through Facebook. The
   reply's top-level `sessionid` is saved to `Session.dat` and `list` (credentials) to
   `Credentials.dat`, which gives the game its client id.
3. From then on requests carry `user.id` and `header.sessionid`, so the server knows which player
   is asking. Later launches resume the saved session; if it's missing, the bridge runs the
   device-level login again (it restores the account e-mail from `NgsLogin.dat` first, since the
   game only knows it after a Glu login).

Once the game has a client id it keeps the profile in `<clientid>_<store>` files instead of
`-1_<store>`. It only migrates the offline `-1_*` files under some conditions, so a new account can
start with an empty profile; copy the old files over with the client id rewritten (the id is
stored just before each file's CRC).

## Friend codes

Every account gets a friend code (`XXXX-XXXX`, no look-alike characters). Friendships are mutual
and stored in `accounts.json`; the game's friends list (`friend/search`) returns them, so they show
up in Brotherhood. Two extra endpoints, outside Glu's API, are for the in-game "Add a Bro" popup:
`PUT /gunbros/friendcode {"id": N}` and `PUT /gunbros/addfriend {"id": N, "code": "XXXX-XXXX"}`.

Admin commands:

```bash
python3 server/ngs_server.py accounts                 # players, ids, friend codes, friends
python3 server/ngs_server.py befriend <id> <code>     # via the running server
```

## Profile data

The game keeps its profile in data stores (`<clientid>_1000` ... `_1019`), and each store's sync
status in `<clientid>_PDST` (one byte per store: 1 = changed locally, 2 = in sync, 4 = load from
the server first). When the profile is online, `CProfileManager::Save` uploads changed stores as
attribute lists (`{name, type, collectiontype, stringValue | intValue}`, collection type = store
number) and marks them 2. Stores in state 4 are never uploaded; the game only reloads them at
startup, before the bridge has signed in. For an existing save, set its statuses to 1 to upload it.

Still open: stores in state 4 on a new account, and what Brotherhood shows from friends' data.

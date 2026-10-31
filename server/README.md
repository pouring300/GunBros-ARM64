# Replacement NGS server

Gun Bros talked to Glu's own backend ("NGS") at `http://ng-gunbros-android.glu.com/gbro/` for
server time, Glu accounts, friends ("brothers"), messages, leaderboards and more. That server is
gone. `ngs_server.py` is a small stand-in that answers the requests reverse-engineered so far and
logs everything else, so the API can be mapped one request at a time.

## Pointing the game at a server

The server the bridge uses is built in with two build settings (environment variables or `.env`):

```bash
GUNBROS_NGS_HOST=ngs.example.com ./build.sh   # host name or IPv4
GUNBROS_NGS_PORT=18080 ./build.sh                               # optional, default 80
```

With no host set the game stays offline, as it was with Glu's servers gone. With one, the bridge
resolves `*.glu.com` to it, rewrites the HTTP `Host` header to it (some hosting platforms route
by that header), pings it at startup to wake it, and creates a Glu account automatically on first
launch (the Android port never shows the game's register dialog).

For testing, system properties override the built-in server:

```bash
python3 server/ngs_server.py --port 18080 --capture server/capture.jsonl
adb reverse tcp:18080 tcp:18080
adb shell setprop debug.gunbros.ngs_host 127.0.0.1    # "off" forces offline
adb shell setprop debug.gunbros.ngs_port 18080
# restart the game; `adb shell setprop debug.gunbros.ngs_host ""` goes back to the built-in server
```

Runtime files, in `--data-dir` (default: next to the script): `accounts.json` (accounts; passwords
stored as salted SHA-256) and `admin.token`. `--capture FILE` logs every request (passwords and
private profile data removed) for debugging.

## Hosting the server

The server is a single Python 3 file with no dependencies, so it runs on almost any host: a small
VM, a container, or a platform-as-a-service that runs Python.

```bash
python3 ngs_server.py --bind 0.0.0.0 --data-dir /path/to/persistent/storage
```

- **Port:** taken from `$PORT` (set by many hosting platforms), otherwise `--port`, default 18080.
  The bridge connects on port 80 unless the APK is built with `GUNBROS_NGS_PORT`.
- **Plain HTTP:** the game can't speak HTTPS, so the host must accept plain HTTP on that port
  (turn off any "HTTPS only" redirect).
- **Data:** `--data-dir` must survive restarts and redeploys; it holds `accounts.json` and
  `admin.token`.
- **Platform quirks the bridge already handles:** hosts that route requests by the HTTP `Host`
  header (the bridge rewrites it to the server's name), and hosts that sleep when idle (the bridge
  pings the server at startup to wake it).

Build the APK with `GUNBROS_NGS_HOST=<server host name> ./build.sh`. On a test phone, clear any
override first: `adb shell "setprop debug.gunbros.ngs_host ''"`.

To move existing accounts to a new server, stop both servers, copy `accounts.json` into the new
server's `--data-dir`, and start it again.

Admin commands run on the host, with the same `--data-dir`: `accounts` lists players, and
`befriend` / `unfriend` (which talk to the running server, so they also need `--port`) make or
end a friendship. Back up `accounts.json` from time to time.

The host name is built into the APK, so moving to another host means a new APK unless the name
stays the same; a domain of your own, pointed at the server, avoids that.

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
| `resources/avatar/avatarattributes` | keeps the public part of the player's profile (level, loadout) |
| `resources/avatar/queryattributes` | returns a friend's public profile, for Brotherhood |
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
up in Brotherhood.

In the game, the "Invite more friends" banner on the Bros tab opens "Add a Bro" (the bridge reuses
the OpenSpy mod's popups): the title shows the player's own code, and entering a friend's code adds
them; Brotherhood reloads its list right away. The bridge uses
two endpoints outside Glu's API, authenticated with the player's NGS session id:
`PUT /gunbros/friendcode {"session": "..."}` (the player's code and name) and
`PUT /gunbros/addfriend {"session": "...", "code": "XXXX-XXXX"}`.

The popup's "Name" button sets the name a player's Bros see (Brotherhood, Bro-Ops recruits and
requests): `PUT /gunbros/setname {"session": "...", "name": "..."}`, 2-12 letters, digits, spaces,
`-`, `_` or `.`. The game shows it as the player's nickname; until one is set, that is the account's
generated gamertag.

Admin commands:

```bash
python3 server/ngs_server.py accounts                 # players, ids, friend codes, friends
python3 server/ngs_server.py befriend <id> <code>     # via the running server (uses server/admin.token)
python3 server/ngs_server.py unfriend <id> <id>       # end a friendship, the same way
```

## Profile data

The game is played offline: the save files on the phone are the only copy of a player's progress,
and the server is not a cloud save. It only keeps what friends are shown in Brotherhood (level,
XP and loadout), returns it only to that player's friends, and never sends a player's own data
back to them.

The game keeps its profile in data stores (`<clientid>_1000` ... `_1019`), and each store's sync
status in `<clientid>_PDST` (one byte per store: 1 = changed locally, 2 = in sync, 4 = load from
the server first). When the profile is online, `CProfileManager::Save` uploads changed stores as
attribute lists (`{name, type, collectiontype, stringValue | intValue}`, collection type = store
number); the server drops everything but the public attributes. The game rejects a friend's
progress unless all of its fields are present, so the private ones (coins etc.) are sent as zeros. At sign-in the bridge marks stores
in state 4 as changed locally, so they are uploaded rather than waiting for server data.

Brotherhood's once-a-day gift per Bro depends on when each Bro was last used. The original game
kept that on Glu's server among the player's own attributes; here the bridge keeps it on the
phone (`BroUsage.dat` next to the save files) and the server doesn't store it.

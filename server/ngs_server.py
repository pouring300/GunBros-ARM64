#!/usr/bin/env python3
"""Minimal stand-in for Glu's Gun Bros "NGS" backend (http://ng-gunbros-android.glu.com/gbro/).

Usage on the PC, with the phone on USB:

    python3 ngs_server.py --port 18080
    adb reverse tcp:18080 tcp:18080
    adb shell setprop debug.gunbros.ngs_host 127.0.0.1
    adb shell setprop debug.gunbros.ngs_port 18080
    (restart the game)

Hosted: python3 ngs_server.py --bind 0.0.0.0 --data-dir /path/to/persistent/storage
(the port comes from $PORT). See README.md.

Wire format (both directions), JSON over HTTP PUT:
    {"payload": [{"version": 1, "sequenceId": N, "message": {...}}]}
Requests carry message.header {device, game, sessionid, mac, type}; replies are matched to
requests by sequenceId. A reply message containing "errorcode" is treated as a failure; any other
reply must contain "list" (the game reads it unconditionally).

With --capture FILE, every request is appended to FILE (passwords and private profile data
removed). Endpoints without a handler get 404.
"""
import argparse
import datetime
import hashlib
import json
import os
import secrets
import threading
import time
from collections import defaultdict, deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HANDLERS = {}


def endpoint(path):
    def register(fn):
        HANDLERS[path] = fn
        return fn
    return register


class Accounts:
    """Glu accounts, persisted to a JSON file. Passwords are stored as salted SHA-256."""

    CODE_ALPHABET = '23456789ABCDEFGHJKMNPQRSTUVWXYZ'  # no 0/O, 1/I/L look-alikes

    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        try:
            with open(path) as f:
                self.data = json.load(f)
        except (OSError, ValueError):
            self.data = {'next_id': 1000, 'users': {}}
        with self.lock:
            for user in self.data['users'].values():
                user.setdefault('code', self._new_code())
                user.setdefault('friends', [])
                # Drop anything stored before only public attributes were kept.
                avatar = {t: {n: a for n, a in attrs.items() if is_public_attribute(a)}
                          for t, attrs in user.get('avatar', {}).items()}
                user['avatar'] = {t: attrs for t, attrs in avatar.items() if attrs}
            self._save()

    def _new_code(self):
        taken = {u.get('code') for u in self.data['users'].values()}
        while True:
            raw = ''.join(secrets.choice(self.CODE_ALPHABET) for _ in range(8))
            code = raw[:4] + '-' + raw[4:]
            if code not in taken:
                return code

    @staticmethod
    def normalize_code(code):
        raw = ''.join(c for c in (code or '').upper() if c.isalnum())
        return raw[:4] + '-' + raw[4:] if len(raw) == 8 else None

    def by_id(self, user_id):
        with self.lock:
            return next((u for u in self.data['users'].values() if u['id'] == user_id), None)

    def befriend(self, user, code):
        """Make the owner of `code` and `user` friends. Returns (friend, error)."""
        code = self.normalize_code(code)
        with self.lock:
            other = next((u for u in self.data['users'].values() if u.get('code') == code), None) if code else None
            if other is None:
                return None, 'Code not found.'
            if other['id'] == user['id']:
                return None, "That's your own code."
            if other['id'] in user['friends']:
                return other, None
            user['friends'].append(other['id'])
            other['friends'].append(user['id'])
            self._save()
            return other, None

    def set_name(self, user, name):
        with self.lock:
            user['name'] = name
            self._save()

    def unfriend(self, user, other_id):
        """End the friendship between `user` and player `other_id`. Returns an error or None."""
        with self.lock:
            other = next((u for u in self.data['users'].values() if u['id'] == other_id), None)
            if other is None or other_id not in user['friends']:
                return 'Not friends.'
            user['friends'].remove(other_id)
            if user['id'] in other.get('friends', []):
                other['friends'].remove(user['id'])
            self._save()
            return None

    def post_message(self, sender, recipient_id, msg_type, text, duration):
        """Queue a message for a friend. Returns False if the recipient isn't one."""
        with self.lock:
            if recipient_id not in sender.get('friends', []):
                return False
            now = int(time.time())
            expires = now + max(60, min(duration, MAX_MESSAGE_DURATION))
            inbox = [m for m in self.data.setdefault('messages', [])
                     if m['recipientid'] == recipient_id and m['expires'] > now]
            # Every mission played with a friend's Bro sends that friend an XP gift; add it to
            # an unread one from the same player instead of filling their inbox (Bro-Ops
            # messages would be turned away once it is full).
            if msg_type == MESSAGE_XP_GIFT:
                xp = xp_gift_amount(text)
                pending = next((m for m in inbox if m['type'] == MESSAGE_XP_GIFT and
                                m['senderid'] == sender['id']), None)
                if xp is not None and pending is not None and xp_gift_amount(pending['message']) is not None:
                    pending['message'] = json.dumps({'xp': xp_gift_amount(pending['message']) + xp})
                    pending['expires'] = max(pending['expires'], expires)
                    self._save()
                    return True
            if len(inbox) >= MAX_PENDING_MESSAGES:
                return False
            self.data['next_message_id'] = self.data.get('next_message_id', 1) + 1
            self.data['messages'].append({
                'id': self.data['next_message_id'] - 1, 'senderid': sender['id'],
                'senderGamertag': display_name(sender), 'recipientid': recipient_id, 'type': msg_type,
                'message': text, 'expires': expires})
            self._save()
            return True

    def inbox(self, user):
        """Unexpired messages for `user`, dropping expired ones."""
        with self.lock:
            now = int(time.time())
            messages = self.data.setdefault('messages', [])
            live = [m for m in messages if m['expires'] > now]
            if len(live) != len(messages):
                self.data['messages'] = live
                self._save()
            return [{k: v for k, v in m.items() if k != 'expires'}
                    for m in live if m['recipientid'] == user['id']]

    def ack_messages(self, user, ids):
        with self.lock:
            messages = self.data.setdefault('messages', [])
            keep = [m for m in messages if not (m['recipientid'] == user['id'] and m['id'] in ids)]
            if len(keep) != len(messages):
                self.data['messages'] = keep
                self._save()

    def friends_of(self, user):
        with self.lock:
            return [u for u in self.data['users'].values() if u['id'] in user.get('friends', [])]

    def _save(self):
        tmp = self.path + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.data, f, indent=1)
        os.replace(tmp, self.path)

    @staticmethod
    def _hash(password, salt):
        return hashlib.sha256((salt + password).encode()).hexdigest()

    def register(self, gamertag, email, password):
        with self.lock:
            if not gamertag or gamertag.lower() in self.data['users']:
                return None
            uid = self.data['next_id']
            self.data['next_id'] += 1
            salt = secrets.token_hex(8)
            self.data['users'][gamertag.lower()] = {
                'id': uid, 'gamertag': gamertag, 'email': email,
                'salt': salt, 'password': self._hash(password, salt), 'created': int(time.time()),
                'code': self._new_code(), 'friends': []}
            self._save()
            return self.data['users'][gamertag.lower()]

    def new_session(self, user):
        with self.lock:
            token = secrets.token_hex(16)
            self.data.setdefault('sessions', {})[token] = user['gamertag'].lower()
            self._save()
            return token

    def by_session(self, token):
        with self.lock:
            name = self.data.get('sessions', {}).get(token or '')
            return self.data['users'].get(name) if name else None

    def find(self, user_id=None, email=None):
        with self.lock:
            for u in self.data['users'].values():
                if (user_id not in (None, -1) and u['id'] == user_id) or (email and u['email'] == email):
                    return u
            return None

    def set_attributes(self, user, attrs):
        with self.lock:
            store = user.setdefault('attributes', {})
            for a in attrs:
                name = a.get('attributeName')
                if name:
                    store[name] = {k: v for k, v in a.items() if k != 'attributeName'}
            self._save()

    def save_avatar_attributes(self, user, attrs):
        """Store the public avatar attributes (see is_public_attribute), replacing older values."""
        with self.lock:
            store = user.setdefault('avatar', {})
            for a in attrs:
                if is_public_attribute(a):
                    store.setdefault(str(a['collectiontype']), {})[a['name']] = a
            self._save()

    def avatar_attributes(self, user, collection_types):
        with self.lock:
            store = user.get('avatar', {})
            return [a for t in collection_types for a in store.get(str(t), {}).values()]

    def validate(self, name, password):
        with self.lock:
            user = self.data['users'].get((name or '').lower())
            if user is None:  # allow logging in by email too
                user = next((u for u in self.data['users'].values() if u['email'] == name), None)
            if user and user['password'] == self._hash(password or '', user['salt']):
                return user
            return None


# A player's name as their Bros see it (the game shows the "nickname"): one they chose, else the
# account's generated gamertag. The gamertag itself stays: it is the login name.
NAME_MAX = 12
NAME_CHARS = set('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_.')


def display_name(user):
    return user.get('name') or user['gamertag']


def clean_name(name):
    """A name as players may set it, or None: 2-12 letters, digits, spaces, '-', '_', '.'."""
    if not isinstance(name, str):
        return None
    name = ' '.join(name.split())
    if not 2 <= len(name) <= NAME_MAX or not set(name) <= NAME_CHARS or not any(c.isalnum() for c in name):
        return None
    return name


def credentials(user):
    # Fields read by CNGSUserCredentials::InitFromObjectRepresentation.
    return {'id': user['id'], 'clientid': user['id'], 'gamertag': user['gamertag'],
            # avatarid > 0: CNGSUserCredentials::hasValidAvatar, required for friends-list entries
            'nickname': display_name(user), 'email': user['email'], 'avatarid': user['id'],
            'socialnetworkname': 'glu', 'socialnetworkid': user['email'],
            'gluid': user['email'], 'gluAlias': user['gamertag']}


def error(code, text='error'):
    # CNGSServerObject::WasErrorInResponse: "errorcode" marks a failure (1008-1011 have special
    # meanings); for other codes it reads "list" as the error text and dereferences it
    # unconditionally, so it must be present.
    return {'errorcode': str(code), 'list': text}


def session_reply(user):
    """Reply body shared by register/validate.

    Two login chains use these endpoints: CGluSocialManager reads the top-level "gamertag"
    (registration) or "list" (validation, a credentials object). CNGSLocalUser reads the
    top-level "sessionid" (CNGSSessionConfig, saved to Session.dat) and "list" (a credentials
    object, or an array whose last element is used; saved to Credentials.dat).
    """
    return {'sessionid': ACCOUNTS.new_session(user)}


@endpoint('/gbro/resources/user/registeruser/')
def register_user(message):
    u = message.get('user', {})
    user = ACCOUNTS.register(u.get('gamertag'), u.get('email'), u.get('password'))
    if user is None:
        return error(1)
    reply = session_reply(user)
    # CGluSocialManager::HandleCreateAccountResponse compares reply "gamertag" with the name it
    # registered.
    reply['gamertag'] = user['gamertag']
    reply['list'] = credentials(user)
    return reply


@endpoint('/gbro/resources/user/validateuser/')
def validate_user(message):
    attr = message.get('userattribute')
    if attr:
        # CNGSLocalUser::ValidateUser (device-level login after a social login) identifies the
        # player by a social-network attribute, e.g. GLUSOCIALID = the account's e-mail.
        user = ACCOUNTS.find(email=attr.get('stringValue')) if attr.get('attributeName') == 'GLUSOCIALID' else None
    else:
        # CGluSocialManager::ValidateUser: gamertag/e-mail and password.
        u = message.get('user', {})
        user = ACCOUNTS.validate(u.get('gamertag') or u.get('email'), u.get('password'))
    if user is None:
        return error(1, 'unknown user')
    reply = session_reply(user)
    reply['list'] = credentials(user)
    return reply


@endpoint('/gbro/resources/user/attributes/')
def update_attributes(message):
    # Sent right after registration with user.id == -1 and the new account's GLUSOCIALID (email).
    attrs = message.get('userattributes', [])
    social = next((a.get('stringValue') for a in attrs if a.get('attributeName') == 'GLUSOCIALID'), None)
    user_id = message.get('user', {}).get('id', -1)
    user = ACCOUNTS.find(user_id=user_id, email=social)
    if user is None:
        # Sent before the game knows its account id (user.id == -1), e.g. GLUSOCIALALIAS right
        # after registration. Accept it as a no-op rather than fail.
        print(f'   attributes for unknown user ignored: {[a.get("attributeName") for a in attrs]}', flush=True)
    else:
        ACCOUNTS.set_attributes(user, attrs)
    return {'list': {}}


@endpoint('/gbro/resources/friend/search')
def friend_search(message):
    # CNGSLocalUser::UpdateFriendListFromServer -> HandleValidateFriendListFromServerResponse:
    # "list" is an array of credentials objects; entries with clientid >= 1 and avatarid > 0
    # become friends ("brothers").
    user = ACCOUNTS.by_id(message.get('user', {}).get('id'))
    friends = ACCOUNTS.friends_of(user) if user else []
    # An empty JSON array decodes as nothing in the game, so send {} when there are no friends.
    return {'list': [credentials(f) for f in friends] if friends else {}}


# Messages between friends (resources/message/*). Bro-Ops uses them: recruiting a Bro posts
# {"challenge", "progress", "timeRange"} (type 1005) to them, and their game replies the same way.
# A mission played with a friend's Bro on duty sends that friend an XP gift (type 1001). The
# server only relays them between friends; apart from adding up XP gifts it doesn't read them.
MAX_PENDING_MESSAGES = 100        # per recipient
MESSAGE_XP_GIFT = 1001            # {"xp": N}: XP a friend's Bro earned in your mission
MAX_MESSAGE_DURATION = 7 * 86400  # seconds
MAX_MESSAGE_TEXT = 2048


def xp_gift_amount(text):
    """The XP in an XP gift message, or None if it isn't one."""
    try:
        xp = json.loads(text).get('xp')
    except (ValueError, AttributeError):
        return None
    return xp if isinstance(xp, int) and not isinstance(xp, bool) and 0 <= xp < 1 << 30 else None


def message_caller(message):
    """The signed-in player making a resources/message/* request (by session id)."""
    return ACCOUNTS.by_session(message.get('header', {}).get('sessionid'))


@endpoint('/gbro/resources/message/post')
def message_post(message):
    # CNGSFromServerMessageQ::PostEnvelopes: {"messages": [{recipientid, duration, type, pushok,
    # message (a string of JSON)}], "user": {...}}. The reply is only checked for errors.
    sender = message_caller(message)
    if sender is None:
        return error(1, 'not signed in')
    for m in message.get('messages', []):
        text, recipient = m.get('message'), m.get('recipientid')
        if not isinstance(text, str) or len(text) > MAX_MESSAGE_TEXT or not isinstance(recipient, int):
            continue
        ACCOUNTS.post_message(sender, recipient, int(m.get('type', 0)), text, int(m.get('duration', 86400)))
    return {'list': {}}


@endpoint('/gbro/resources/message/poll')
def message_poll(message):
    # CNGSFromServerMessageQ::CheckForEnvelopesFromServer, every 15 s: "list" is an array of
    # envelopes {id, type, message, senderid, recipientid, senderGamertag} ({} when empty). The
    # game acks the ones it has handled; the rest are delivered again until they expire.
    user = message_caller(message)
    envelopes = ACCOUNTS.inbox(user) if user else []
    return {'list': envelopes if envelopes else {}}


@endpoint('/gbro/resources/message/ack')
def message_ack(message):
    # {"messages": [{"id": N}, ...]}: delivered and handled, so delete them.
    user = message_caller(message)
    if user:
        ids = {m.get('id') for m in message.get('messages', []) if isinstance(m, dict)}
        ACCOUNTS.ack_messages(user, ids)
    return {'list': {}}


# The game is played offline and its save files stay the only copy of a player's progress: the
# server is not a cloud save. It keeps only what friends are shown in Brotherhood (level and
# loadout) and never sends a player's own data back to them.
PUBLIC_ATTRIBUTES = {
    1000: lambda name: name in ('gbLevel', 'gbExperience'),   # player progress
    1001: lambda name: name.startswith('gbPlayCfg'),          # loadout: character, guns, armor
}


# CPlayerProgress::LoadFromServer rejects a friend's progress unless every one of these is present,
# so the private ones are sent as zeros: (name, type) as the game saves them.
PROGRESS_PLACEHOLDERS = [('gbFirstLaunch', 1), ('gbExplodium', 4), ('gbCurrencyCommon', 4),
                         ('gbCurrencyRare', 3), ('gbXPBonusTimestamp', 3), ('gbXPBonusQuantity', 3),
                         ('gbPushChallenges', 1), ('gbExperience', 4), ('gbLevel', 2)]


def with_placeholders(attrs, collection_types):
    if 1000 not in collection_types:
        return attrs
    have = {a['name'] for a in attrs if a.get('collectiontype') == 1000}
    return attrs + [{'name': n, 'type': t, 'collectiontype': 1000, 'intValue': 0}
                    for n, t in PROGRESS_PLACEHOLDERS if n not in have]


def is_public_attribute(attr):
    check = PUBLIC_ATTRIBUTES.get(attr.get('collectiontype'))
    return bool(check and isinstance(attr.get('name'), str) and check(attr['name']))


def avatar_owner(message):
    """The account a resources/avatar/* request is about (its avatar id is the account id)."""
    avatar_id = message.get('registeravatar', {}).get('id', message.get('user', {}).get('id'))
    return ACCOUNTS.by_id(avatar_id)


@endpoint('/gbro/resources/avatar/avatarattributes')
def avatar_save_attributes(message):
    # CAttributeManager::saveDataToServer: the player's profile data, as
    # {"avatarattributes": [{name, type, collectiontype, stringValue | intValue}, ...]}.
    # Only the public part is kept, and only the signed-in player may change their own.
    user = avatar_owner(message)
    caller = ACCOUNTS.by_session(message.get('header', {}).get('sessionid'))
    if user is None or caller is None or caller['id'] != user['id']:
        return error(1, 'not signed in')
    ACCOUNTS.save_avatar_attributes(user, message.get('avatarattributes', []))
    return {'list': {}}


@endpoint('/gbro/resources/avatar/queryattributes')
def avatar_query_attributes(message):
    # CAttributeManager::getDataFromServer asks for a player's attributes by collection type
    # ({"avatarattributes": [{"collectiontype": N}, ...]}); the reply's "list" is an array of
    # attribute objects in the same form they were saved in (CAttributeManager::
    # extractAttributeValuesAsArray). Friends' data is read this way for Brotherhood.
    # Only friends' data is returned: a player's own data stays on their phone.
    user = avatar_owner(message)
    caller = ACCOUNTS.by_session(message.get('header', {}).get('sessionid'))
    attrs = []
    if user and caller and user['id'] in caller.get('friends', []):
        types = [a.get('collectiontype') for a in message.get('avatarattributes', [])]
        attrs = with_placeholders(ACCOUNTS.avatar_attributes(user, types), types)
    return {'list': attrs if attrs else {}}


@endpoint('/gbro/resources/user/getnettime2')
def get_net_time(message):
    # CNGSSession::handleResponseNetworkTime reads message["list"] as an int64 (ms since 1970).
    return {'list': int(time.time() * 1000)}


def redacted(value):
    """Copy of a request body for the capture log, without passwords or private profile data."""
    if isinstance(value, dict):
        out = {k: ('<redacted>' if k.lower() == 'password' else redacted(v)) for k, v in value.items()}
        if isinstance(out.get('avatarattributes'), list):
            out['avatarattributes'] = [a for a in out['avatarattributes']
                                       if 'name' not in a or is_public_attribute(a)]
        return out
    if isinstance(value, list):
        return [redacted(v) for v in value]
    return value


def ext_player(body):
    """The player an extension request is for: by session id (the game), or by account id with
    the admin token (the `befriend` command)."""
    if body.get('session'):
        return ACCOUNTS.by_session(body['session'])
    if ADMIN_TOKEN and body.get('admin') == ADMIN_TOKEN:
        return ACCOUNTS.by_id(body.get('id'))
    return None


def ext_friendcode(body):
    user = ext_player(body)
    if user is None:
        return {'ok': False, 'error': 'Not signed in.'}
    return {'ok': True, 'code': user['code'], 'gamertag': user['gamertag'], 'name': display_name(user)}


def ext_setname(body):
    user = ext_player(body)
    if user is None:
        return {'ok': False, 'error': 'Not signed in.'}
    name = clean_name(body.get('name'))
    if name is None:
        return {'ok': False, 'error': f'2-{NAME_MAX} letters or digits.'}
    ACCOUNTS.set_name(user, name)
    return {'ok': True, 'name': name}


def ext_addfriend(body):
    user = ext_player(body)
    if user is None:
        return {'ok': False, 'error': 'Not signed in.'}
    friend, err = ACCOUNTS.befriend(user, body.get('code'))
    return {'ok': True, 'gamertag': friend['gamertag']} if friend else {'ok': False, 'error': err}


def ext_unfriend(body):
    # Admin only (the `unfriend` command); the game has no way to remove a Bro.
    if not (ADMIN_TOKEN and body.get('admin') == ADMIN_TOKEN):
        return {'ok': False, 'error': 'Not allowed.'}
    user = ext_player(body)
    if user is None or not isinstance(body.get('friend'), int):
        return {'ok': False, 'error': 'No such player.'}
    err = ACCOUNTS.unfriend(user, body['friend'])
    return {'ok': True} if err is None else {'ok': False, 'error': err}


def ext_ping(body):
    return {'ok': True}


# Endpoints used by the bridge itself: plain JSON in, plain JSON out.
EXT_HANDLERS = {'/gunbros/friendcode': ext_friendcode, '/gunbros/addfriend': ext_addfriend,
                '/gunbros/setname': ext_setname,
                '/gunbros/unfriend': ext_unfriend, '/gunbros/ping': ext_ping}
ADMIN_TOKEN = None


MAX_BODY = 64 * 1024
REGISTRATIONS_PER_HOUR = 10  # per client address


class RateLimiter:
    """At most `limit` events per `window` seconds per key."""

    def __init__(self, limit, window):
        self.limit, self.window = limit, window
        self.events = defaultdict(deque)
        self.lock = threading.Lock()

    def allow(self, key):
        now = time.monotonic()
        with self.lock:
            q = self.events[key]
            while q and now - q[0] > self.window:
                q.popleft()
            if len(q) >= self.limit:
                return False
            q.append(now)
            return True


REGISTRATIONS = RateLimiter(REGISTRATIONS_PER_HOUR, 3600)


class Handler(BaseHTTPRequestHandler):
    server_version = 'GunBrosNGS/0.1'

    def _client(self):
        # Behind a reverse proxy or a hosting platform's front end, the client is the first forwarded address.
        forwarded = self.headers.get('X-Forwarded-For', '')
        return forwarded.split(',')[0].strip().rsplit(':', 1)[0] if forwarded else self.client_address[0]

    def _reply(self, status, data=b''):
        self.send_response(status)
        if data:
            self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _handle(self):
        try:
            length = int(self.headers.get('Content-Length') or 0)
        except ValueError:
            length = -1
        if length < 0 or length > MAX_BODY:
            self._reply(413)
            return
        raw = self.rfile.read(length) if length else b''
        try:
            body = json.loads(raw.decode('utf-8')) if raw else None
        except (UnicodeDecodeError, ValueError):
            body = None

        path = self.path.split('?', 1)[0]
        if path in EXT_HANDLERS:
            result = EXT_HANDLERS[path](body if isinstance(body, dict) else {})
            print(f"{datetime.datetime.now().isoformat(timespec='milliseconds')} {path} -> {json.dumps(result)}", flush=True)
            self._reply(200, json.dumps(result).encode())
            return
        handler = HANDLERS.get(path)
        if handler is register_user and not REGISTRATIONS.allow(self._client()):
            handler = lambda message: error(1, 'too many new accounts, try again later')
        reply = None
        if handler and isinstance(body, dict) and body.get('payload'):
            replies = []
            for item in body['payload']:
                message = handler(item.get('message', {}))
                # CNGSServerObject::WasErrorInResponse dereferences message["list"] without a
                # null check, so every successful reply must carry one (else: jump to address 0).
                if 'errorcode' not in message:
                    message.setdefault('list', {})
                replies.append({'version': item.get('version', 1),
                                'sequenceId': item.get('sequenceId', 0),
                                'message': message})
            reply = {'payload': replies}

        stamp = datetime.datetime.now().isoformat(timespec='milliseconds')
        record = {'time': stamp, 'method': self.command, 'path': self.path,
                  'headers': dict(self.headers), 'body': redacted(body) if body is not None else raw.decode('latin-1'),
                  'reply': reply}
        if self.server.capture_path:
            with open(self.server.capture_path, 'a') as f:
                f.write(json.dumps(record) + '\n')

        msg = (body or {}).get('payload', [{}])[0].get('message', {}) if isinstance(body, dict) else {}
        msg_type = msg.get('header', {}).get('type', '?') if isinstance(msg, dict) else '?'
        status = 'handled' if reply else '404'
        print(f'{stamp} {self.command} {path} type={msg_type} -> {status}', flush=True)

        if reply is None:
            self._reply(404)
            return
        self._reply(200, json.dumps(reply, separators=(',', ':')).encode())

    do_GET = do_POST = do_PUT = do_DELETE = _handle

    def log_message(self, fmt, *args):
        pass


def cli(args):
    """Small admin commands. `befriend` and `unfriend` go through the running server so its state
    stays in sync."""
    if args.command == 'accounts':
        accounts = Accounts(os.path.join(args.data_dir, 'accounts.json'))
        for u in sorted(accounts.data['users'].values(), key=lambda u: u['id']):
            print(f"{u['id']:>6}  {u['code']}  {u['gamertag']:<20} {u.get('name', ''):<{NAME_MAX}}  "
                  f"friends={u.get('friends', [])}")
    elif args.command in ('befriend', 'unfriend'):
        import urllib.request
        if len(args.params) != 2:
            raise SystemExit(f'usage: {args.command} <player id> '
                             f'{"<friend code>" if args.command == "befriend" else "<friend id>"}')
        with open(admin_token_path(args)) as f:
            token = f.read().strip()
        body = {'id': int(args.params[0]), 'admin': token}
        if args.command == 'befriend':
            path, body['code'] = '/gunbros/addfriend', args.params[1]
        else:
            path, body['friend'] = '/gunbros/unfriend', int(args.params[1])
        req = urllib.request.Request(f'http://127.0.0.1:{args.port}{path}', method='PUT',
                                     data=json.dumps(body).encode())
        print(urllib.request.urlopen(req).read().decode())


def admin_token_path(args):
    return os.path.join(args.data_dir, 'admin.token')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', nargs='?', choices=['serve', 'accounts', 'befriend', 'unfriend'], default='serve',
                    help='serve (default); accounts: list players and friend codes; '
                         'befriend <player id> <friend code>: make two players friends; '
                         'unfriend <player id> <friend id>: end a friendship')
    ap.add_argument('params', nargs='*')
    ap.add_argument('--bind', default='127.0.0.1', help='address to listen on (0.0.0.0 when hosted)')
    ap.add_argument('--port', type=int, default=int(os.environ.get('PORT', 18080)),
                    help='port to listen on (default: $PORT, else 18080)')
    ap.add_argument('--data-dir', default=os.path.dirname(os.path.abspath(__file__)),
                    help='where accounts.json and admin.token live (default: next to this script)')
    ap.add_argument('--capture', metavar='FILE', help='append every request to FILE (debugging)')
    args = ap.parse_args()
    if args.command != 'serve':
        return cli(args)
    global ACCOUNTS
    os.makedirs(args.data_dir, exist_ok=True)
    ACCOUNTS = Accounts(os.path.join(args.data_dir, 'accounts.json'))
    global ADMIN_TOKEN
    ADMIN_TOKEN = secrets.token_hex(16)
    fd = os.open(admin_token_path(args), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(ADMIN_TOKEN + '\n')
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    server.capture_path = args.capture
    print(f'NGS stand-in on {args.bind}:{args.port}, data in {args.data_dir}'
          f'{", capturing to " + args.capture if args.capture else ""}', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()

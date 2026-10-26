#!/usr/bin/env python3
"""Minimal stand-in for Glu's Gun Bros "NGS" backend (http://ng-gunbros-android.glu.com/gbro/).

Usage on the PC, with the phone on USB:

    python3 ngs_server.py --port 18080
    adb reverse tcp:18080 tcp:18080
    adb shell setprop debug.gunbros.ngs_host 127.0.0.1
    adb shell setprop debug.gunbros.ngs_port 18080
    (restart the game)

Wire format (both directions), JSON over HTTP PUT:
    {"payload": [{"version": 1, "sequenceId": N, "message": {...}}]}
Requests carry message.header {device, game, sessionid, mac, type}; replies are matched to
requests by sequenceId. A reply message containing "errorcode" is treated as a failure; any other
reply must contain "list" (the game reads it unconditionally).

Every request is appended to capture.jsonl. Endpoints without a handler get 404.
"""
import argparse
import datetime
import hashlib
import json
import os
import secrets
import threading
import time
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

    def validate(self, name, password):
        with self.lock:
            user = self.data['users'].get((name or '').lower())
            if user is None:  # allow logging in by email too
                user = next((u for u in self.data['users'].values() if u['email'] == name), None)
            if user and user['password'] == self._hash(password or '', user['salt']):
                return user
            return None


def credentials(user):
    # Fields read by CNGSUserCredentials::InitFromObjectRepresentation.
    return {'id': user['id'], 'clientid': user['id'], 'gamertag': user['gamertag'],
            # avatarid > 0: CNGSUserCredentials::hasValidAvatar, required for friends-list entries
            'nickname': user['gamertag'], 'email': user['email'], 'avatarid': user['id'],
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
    (registration) or "list" (validation, a credentials object),
    while CNGSLocalUser reads /payload/0/message/message = {"sessionid", "list": [credentials]}
    and writes Credentials.dat and Session.dat from it.
    """
    creds = credentials(user)
    return {'message': {'sessionid': ACCOUNTS.new_session(user), 'list': [creds]}}


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


@endpoint('/gbro/resources/message/poll')
def message_poll(message):
    # Inbox check ({"user": {"id": N}}), sent after login. No messaging yet: nothing new.
    return {'list': {}}


@endpoint('/gbro/resources/user/getnettime2')
def get_net_time(message):
    # CNGSSession::handleResponseNetworkTime reads message["list"] as an int64 (ms since 1970).
    return {'list': int(time.time() * 1000)}


def redacted(value):
    """Copy of a request body with passwords removed, for the capture log."""
    if isinstance(value, dict):
        return {k: ('<redacted>' if k.lower() == 'password' else redacted(v)) for k, v in value.items()}
    if isinstance(value, list):
        return [redacted(v) for v in value]
    return value


def ext_friendcode(body):
    user = ACCOUNTS.by_id(body.get('id'))
    return {'ok': True, 'code': user['code'], 'gamertag': user['gamertag']} if user else {'ok': False, 'error': 'Unknown player.'}


def ext_addfriend(body):
    user = ACCOUNTS.by_id(body.get('id'))
    if user is None:
        return {'ok': False, 'error': 'Unknown player.'}
    friend, err = ACCOUNTS.befriend(user, body.get('code'))
    return {'ok': True, 'gamertag': friend['gamertag']} if friend else {'ok': False, 'error': err}


# Endpoints used by the bridge's friend-code popup: plain JSON in, plain JSON out.
EXT_HANDLERS = {'/gunbros/friendcode': ext_friendcode, '/gunbros/addfriend': ext_addfriend}


class Handler(BaseHTTPRequestHandler):
    server_version = 'GunBrosNGS/0.1'

    def _handle(self):
        length = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(length) if length else b''
        try:
            body = json.loads(raw.decode('utf-8')) if raw else None
        except (UnicodeDecodeError, ValueError):
            body = None

        path = self.path.split('?', 1)[0]
        if path in EXT_HANDLERS:
            result = EXT_HANDLERS[path](body if isinstance(body, dict) else {})
            print(f"{datetime.datetime.now().isoformat(timespec='milliseconds')} {path} {json.dumps(body)} -> {json.dumps(result)}", flush=True)
            data = json.dumps(result).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        handler = HANDLERS.get(path)
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
        with open(self.server.capture_path, 'a') as f:
            f.write(json.dumps(record) + '\n')

        msg = (body or {}).get('payload', [{}])[0].get('message', {}) if isinstance(body, dict) else {}
        msg_type = msg.get('header', {}).get('type', '?') if isinstance(msg, dict) else '?'
        status = 'handled' if reply else '404'
        print(f'{stamp} {self.command} {path} type={msg_type} -> {status}', flush=True)

        if reply is None:
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        data = json.dumps(reply, separators=(',', ':')).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    do_GET = do_POST = do_PUT = do_DELETE = _handle

    def log_message(self, fmt, *args):
        pass


def cli(args):
    """Small admin commands. `befriend` goes through the running server so its state stays in sync."""
    if args.command == 'accounts':
        accounts = Accounts(os.path.join(os.path.dirname(os.path.abspath(args.out)), 'accounts.json'))
        for u in sorted(accounts.data['users'].values(), key=lambda u: u['id']):
            print(f"{u['id']:>6}  {u['code']}  {u['gamertag']:<20} friends={u.get('friends', [])}")
    elif args.command == 'befriend':
        import urllib.request
        player_id, code = int(args.params[0]), args.params[1]
        req = urllib.request.Request(f'http://127.0.0.1:{args.port}/gunbros/addfriend', method='PUT',
                                     data=json.dumps({'id': player_id, 'code': code}).encode())
        print(urllib.request.urlopen(req).read().decode())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', nargs='?', choices=['serve', 'accounts', 'befriend'], default='serve',
                    help='serve (default); accounts: list players and friend codes; '
                         'befriend <player id> <friend code>: make two players friends')
    ap.add_argument('params', nargs='*')
    ap.add_argument('--port', type=int, default=18080)
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'capture.jsonl'))
    args = ap.parse_args()
    if args.command != 'serve':
        return cli(args)
    global ACCOUNTS
    ACCOUNTS = Accounts(os.path.join(os.path.dirname(os.path.abspath(args.out)), 'accounts.json'))
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.capture_path = args.out
    print(f'NGS stand-in on 127.0.0.1:{args.port}, capturing to {args.out}; handlers: {sorted(HANDLERS)}', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()

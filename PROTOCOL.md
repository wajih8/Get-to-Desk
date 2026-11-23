# Attended signaling protocol v1

This contract covers the invitation and consent flow in task F03 of the Windows remote desktop plan. It gives the host and viewer a shared session ID and the permissions approved at the host.

## Connection and message format

Connect a native WebSocket client to `ws://127.0.0.1:8765/ws` for the local prototype. Use one UTF-8 JSON object per WebSocket text message, up to 4,096 bytes. An `Origin` header causes the server to reject the upgrade. On an accepted socket the server sends `ready` first.

```json
{"v":1,"type":"ready","peer_id":"10000000000040008000000000000001"}
```

In Qt, use the default empty origin on `QWebSocket` and handle incoming JSON on `textMessageReceived`. [Qt WebSocket documentation](https://doc.qt.io/qt-6/qwebsocket.html)

Each command has `v` (the JSON integer `1`), `type` (the command name) and `request_id` (1 to 64 ASCII letters, digits, underscores or hyphens). Use a fresh `request_id` that your socket can use to match a reply. The server echoes it in the direct reply and omits it in unsolicited events. A request ID correlates messages. Retrying a command runs its checks again.

Send the fields shown for each command, with `ttl_seconds` optional on `create_invitation`. The server rejects unknown or duplicate JSON keys. Server-issued `peer_id`, `invitation_id`, `access_request_id` and `session_id` values are 32 lowercase hexadecimal characters. Treat them as opaque. An `invitation_code` is a case-sensitive, 32-character string of ASCII letters, digits, underscores and hyphens. `expires_at` is a server UTC timestamp in RFC 3339 format with milliseconds and `Z`.

The socket that creates an invitation is its host. The socket that redeems the code is its viewer. The same app can take either role, and a socket cannot redeem its own code. Authorization checks use the live sockets associated with these roles. A reconnect gets a fresh `peer_id`. The viewer supplies the display name, so show it as unverified. Device authentication and WebRTC fingerprint binding are part of the later L02 media integration.

## 1. Create an invitation

The host sends `create_invitation` after choosing a display. `display_id` is a host-local label of 1 to 64 ASCII letters, digits, underscores or hyphens. `ttl_seconds` is a JSON integer from 1 to 300 and defaults to 300.

```json
{"v":1,"type":"create_invitation","request_id":"host-1","display_id":"display-1","ttl_seconds":300}
```

Only the host gets `invitation_created`. Share the `invitation_code` with the intended viewer and keep it out of logs.

```json
{"v":1,"type":"invitation_created","request_id":"host-1","invitation_id":"30000000000040008000000000000003","invitation_code":"Abcdefghijklmnopqrstuvwxyz012345","display_id":"display-1","expires_at":"2026-11-23T12:05:00.000Z"}
```

## 2. Request access

The viewer sends `request_access` with the code. `viewer_name` is 1 to 64 Unicode code points. Its characters must be letters, marks, numbers, punctuation, symbols or ordinary spaces, with no leading or trailing spaces.

```json
{"v":1,"type":"request_access","request_id":"viewer-1","invitation_code":"Abcdefghijklmnopqrstuvwxyz012345","viewer_name":"Viewer PC"}
```

The first successful request claims the invitation and creates one pending approval. The viewer receives `access_pending` with the invitation's original deadline.

```json
{"v":1,"type":"access_pending","request_id":"viewer-1","access_request_id":"40000000000040008000000000000004","invitation_id":"30000000000040008000000000000003","host_peer_id":"10000000000040008000000000000001","expires_at":"2026-11-23T12:05:00.000Z"}
```

The host receives `access_requested`. Prompt the person at the host to choose whether to approve this viewer for the selected display.

```json
{"v":1,"type":"access_requested","access_request_id":"40000000000040008000000000000004","invitation_id":"30000000000040008000000000000003","viewer_peer_id":"20000000000040008000000000000002","viewer_name":"Viewer PC","display_id":"display-1","expires_at":"2026-11-23T12:05:00.000Z"}
```

## 3. Accept access

Only the invitation's host may send `accept_access`, after the person at the host approves. Use the pending `access_request_id`. `permissions` must contain `view` and may also contain `keyboard` and `pointer`, each at most once. A view-only session uses `["view"]`.

```json
{"v":1,"type":"accept_access","request_id":"host-2","access_request_id":"40000000000040008000000000000004","permissions":["view"]}
```

Both peers receive `session_started`. Only the host gets `request_id`. The grant identifies the host display and both sockets. Permissions come back in `view`, `keyboard`, `pointer` order when present. The session has a new deadline two hours from approval.

```json
{"v":1,"type":"session_started","request_id":"host-2","session_id":"50000000000040008000000000000005","access_request_id":"40000000000040008000000000000004","host_peer_id":"10000000000040008000000000000001","viewer_peer_id":"20000000000040008000000000000002","display_id":"display-1","permissions":["view"],"expires_at":"2026-11-23T14:01:00.000Z"}
```

## 4. Decline access

As an alternative to accepting the pending request, the host can send `decline_access`.

```json
{"v":1,"type":"decline_access","request_id":"host-3","access_request_id":"40000000000040008000000000000004"}
```

Both peers receive `access_closed` with `reason` set to `declined`. Only the host gets `request_id`. No session is created. A fresh invitation is required for another attempt.

```json
{"v":1,"type":"access_closed","request_id":"host-3","access_request_id":"40000000000040008000000000000004","invitation_id":"30000000000040008000000000000003","reason":"declined"}
```

The same event format applies when a pending request expires or loses a peer. In those cases `reason` is `invitation_expired`, `host_disconnected` or `viewer_disconnected`, and the server sends it without `request_id` to each connected participant.

## 5. End a session

Either participant can send `end_session` with the active `session_id`.

```json
{"v":1,"type":"end_session","request_id":"viewer-2","session_id":"50000000000040008000000000000005"}
```

Both peers receive `session_ended`. Only the sender gets `request_id`. `reason` is `ended_by_host` or `ended_by_viewer` for an explicit stop. If a peer disconnects or the session reaches its own deadline, the server sends an event without `request_id` to each connected peer with `host_disconnected`, `viewer_disconnected` or `session_expired`.

```json
{"v":1,"type":"session_ended","request_id":"viewer-2","session_id":"50000000000040008000000000000005","reason":"ended_by_viewer"}
```

For five minutes after termination, a repeat `end_session` from either original socket returns the same reason to that socket alone. Ended sessions are terminal. Starting again requires a new invitation and host approval.

## Expiration and disconnects

- The invitation is valid only before its server-side deadline. The server checks expiry on incoming commands and on a timer. If nobody has used the code by the deadline, the host receives `invitation_expired` and can remove the code from the share screen.
- If the invitation expires while approval is pending, both peers receive `access_closed` with `reason` set to `invitation_expired`. Dismiss the approval prompt. A late accept or decline gets `request_not_found`.
- Using a known expired code returns `invitation_expired`. A claimed code returns `invitation_used` while pending and after a decision or disconnect. A pending claim becomes expired if its deadline arrives before a decision. The server forgets invitation records five minutes after their original deadline and then returns `invitation_not_found`.
- If approval arrived before the invitation deadline, the session continues under its own `expires_at`.
- Host disconnect invalidates its open invitations. Disconnect of either peer closes their pending requests and ends their sessions. Restart clears all state.

The `invitation_expired` event for an unused invitation looks like this.

```json
{"v":1,"type":"invitation_expired","invitation_id":"30000000000040008000000000000003"}
```

Claude's desktop client should keep capture and input disabled while awaiting approval, then enforce only the granted permissions. On a local End action, `session_ended`, socket loss or grant expiry, the client must stop capture and remote input locally, release held keys and buttons, and clear queued input and old frames. Local End takes effect immediately while the server notifies the other peer.

## Errors and limits

Each rejected command returns `error` with a machine-readable `code`. The server includes `request_id` whenever it could extract a valid one from the JSON object.

```json
{"v":1,"type":"error","request_id":"viewer-3","code":"invitation_used"}
```

| Code | Meaning |
|---|---|
| `invalid_message` | Invalid JSON, envelope, fields or values, including an unsupported permission. |
| `unsupported_version` | `v` is an integer other than `1`. |
| `unknown_type` | `type` is a string that does not name a v1 command. |
| `invitation_not_found` | The code is unknown or its record was already removed. |
| `invitation_expired` | The code expired before approval. Create a new invitation. |
| `invitation_used` | The code was already claimed or closed. |
| `self_request` | The invitation belongs to the requesting socket. |
| `request_not_found` | The access request is unknown or no longer pending. |
| `session_not_found` | The session ID is unknown or its ended record was removed. |
| `forbidden` | The socket is outside the authorized role for that request or session. |
| `rate_limited` | The socket already attempted 20 validly formatted invitation creations or access requests in the preceding 60 seconds, counted together. |
| `capacity_reached` | The server already holds 2,048 invitations or 2,048 sessions, including retained records of the relevant kind. |

The single-process development server accepts up to 128 connections. Further upgrades are rejected. Binary WebSocket messages close the socket with code `1003`. Oversized messages close it with code `1009`. A stalled outgoing socket is closed with code `1013`.

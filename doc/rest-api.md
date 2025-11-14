# OrcaSlicer Embedded REST API

This API runs inside OrcaSlicer and exposes a small, JSON-based RPC over HTTP for headless automation: import models, slice, send to printer over LAN, query devices, stream logs, and cancel a print.

- Host: 127.0.0.1 (loopback only)
- Port: 8081 (default in GUI)
- Scheme: http (no TLS)
- Path: POST /rpc
- Content-Type: application/json

The server starts automatically when the GUI launches and stops when the GUI exits.

Note: All actions are asynchronous. The immediate HTTP response includes a `task_id` you can poll using the `task_status` action and optional per-task logs via the `get_logs` action.

## Quick start

- Endpoint: `http://127.0.0.1:8081/rpc`
- Request envelope: a single JSON object with an `action` plus action-specific fields.
- Response: JSON with `status`, `message`, `task` snapshot, and `task_id`.

Examples below use curl. On Windows PowerShell, curl is available as an alias; if you prefer, use `Invoke-RestMethod` with the same JSON bodies.

## Actions

### 1) import

Load a model file into OrcaSlicer (same as File > Import).

Request

```
{
  "action": "import",
  "path": "C:\\absolute\\path\\to\\model.stl"
}
```

Response (abridged)

```
{
  "status": "ok",
  "message": "scheduled import",
  "task": { "id": "…", "state": "submitted", "logs": [ … ] },
  "task_id": "…"
}
```

Notes
- `path` must be an absolute path; Windows backslashes need escaping in JSON.

---

### 2) slice_plate

Slice a specific plate in the current project.

Request

```
{
  "action": "slice_plate",
  "plate_index": 0
}
```

- `plate_index` is zero-based.

---

### 3) print_plate

Export the current plate to a 3MF and send a print over LAN to the selected Bambu device, mirroring GUI behavior.

Request

```
{
  "action": "print_plate",
  "plate_index": 0,
  "all": false,
  "dev_id": "0309BA562000423",     // optional; defaults to currently selected printer
  "dev_ip": "192.168.0.109"         // optional; auto-resolved from device if omitted
}
```

- The server derives credentials (username `bblp`, access code) and SSL flags from the selected device.
- Flow: preflight (SD-card check) → local print with record → local print → (cloud fallback if enabled; often blocked)
- On MQTT publish failure, it retries once toggling MQTT SSL and reconnects automatically.

Response (abridged)

```
{
  "status": "ok",
  "message": "scheduled print",
  "task": { "id": "…", "state": "submitted" | "failed" | "success", "logs": [ … ] },
  "task_id": "…"
}
```

Useful task logs include progress (upload MB), return codes, and human-readable mappings for common Bambu networking errors.

---

### 4) cancel_print

Cancel the active print on a device. If `job_id` is omitted, issues an abort of the current task.

Request (cancel a specific job)

```
{
  "action": "cancel_print",
  "dev_id": "0309BA562000423",
  "job_id": "1234567890"
}
```

Request (abort current job on selected device)

```
{
  "action": "cancel_print"
}
```

Response

```
{
  "status": "ok",
  "message": "cancel sent",
  "task": { "id": "…", "state": "success" },
  "task_id": "…"
}
```

---

### 5) devices (list_devices)

List known printers (local and/or user devices).

Request

```
{
  "action": "devices",
  "include_local": true,
  "include_user": true
}
```

Response

```
{
  "status": "ok",
  "message": "ok",
  "devices": [
    { "dev_id": "0309BA562000423", "dev_name": "Mini P", "dev_ip": "192.168.0.109", "connection_type": "lan" }
  ],
  "task_id": "…"
}
```

---

### 6) task_status

Get a snapshot of a previously returned `task_id`.

Request

```
{
  "action": "task_status",
  "id": "<task_id>"
}
```

Response

```
{
  "status": "ok",
  "message": "ok",
  "task": {
    "id": "<task_id>",
    "action": "print_plate",
  "state": "scheduled" | "running" | "submitted" | "success" | "failed",
    "logs": [ "…" ],
    "updated_ms": 123456789
  },
  "task_id": "<task_id>"
}
```

---

### 7) get_logs

Tail the OrcaSlicer log files for diagnostics. By default returns the newest log file, up to 64 KiB.

Request (defaults)

```
{
  "action": "get_logs"
}
```

Request (explicit file and size)

```
{
  "action": "get_logs",
  "file": "debug_2025-11-04.log.0",
  "max_bytes": 131072
}
```

Response

```
{
  "status": "ok",
  "message": "ok",
  "file": "debug_2025-11-04.log.0",
  "content": "…tail text…",
  "truncated": true,
  "size": 987654,
  "returned": 65536,
  "task_id": "…"
}
```

## Error codes and diagnostics

- HTTP level
  - 200 OK for handled actions; body includes `status: ok|error` and `message`.
  - 404 Not Found for non-/rpc paths.
- Task-level
  - `task.state` communicates lifecycle: scheduled → running → submitted → completed | failed.
  - `task.logs[]` captures detailed milestones and any networking codes.
- Bambu networking error mapping (subset)
  - -4020: LP: ftp upload failed
  - -4030: LP: mqtt publish failed
  - -2030/-2110/-2120: WR (with-record) errors (config/3mf upload or post)
  - -3030/-3070/-3120: SP (cloud) errors
  - -6010: connection to printer failed

## Security and scope

- The server binds to 127.0.0.1 and is not exposed externally.
- No authentication; intended for trusted local automation only.

## PowerShell examples (optional)

Import a model

```powershell
$body = '{"action":"import","path":"C:\\Models\\benchy.stl"}'
curl -s -X POST http://127.0.0.1:8081/rpc -H "Content-Type: application/json" -d $body
```

Print the first plate to a specific device

```powershell
$body = '{"action":"print_plate","plate_index":0,"dev_id":"0309BA562000423"}'
curl -s -X POST http://127.0.0.1:8081/rpc -H "Content-Type: application/json" -d $body
```

Poll task status

```powershell
$task = "<task_id>"
$body = '{"action":"task_status","id":"' + $task + '"}'
curl -s -X POST http://127.0.0.1:8081/rpc -H "Content-Type: application/json" -d $body
```

Cancel the current print (selected device)

```powershell
$body = '{"action":"cancel_print"}'
curl -s -X POST http://127.0.0.1:8081/rpc -H "Content-Type: application/json" -d $body
```

## Notes

- Indices: `plate_index` is zero-based in requests; the internal print params convert to one-based for firmware.
- Device resolution: If `dev_id` is omitted, the currently selected device is used. IP, credentials, and SSL preferences are populated from the device profile.
- AMS mapping: The current implementation applies a known-good mapping for your setup; this may become dynamic in a future revision.

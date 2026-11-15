# Authorization and Permissions

## A method needs Polkit if any of these are true:

- Changes system-wide state
- Affects other users
- Persists beyond the session
- Would normally require root
- Is security-relevant
- Is irreversible or destructive

## Examples

| Area     | Method                | Polkit?        |
|----------|-----------------------|----------------|
| Accounts | AddUser               | ✅              |
| Accounts | DeleteUser            | ✅              |
| Accounts | ListUsers             | ❌              |
| Sessions | LockSession           | ❌              |
| Sessions | TerminateOtherSession | ✅              |
| Power    | Suspend               | ⚠️ (often yes) |
| Power    | PowerOff              | ✅              |
| Audio    | SetUserVolume         | ❌              |
| Audio    | SetSystemVolume       | ✅              |
| Network  | EnableWifi            | ⚠️             |
| Network  | AddSystemConnection   | ✅              |


## Notes

This daemon owns the public API and policies. Any D-Bus methods called by this daemon are treated as implementation details,
and should not trigger new authentication flows.
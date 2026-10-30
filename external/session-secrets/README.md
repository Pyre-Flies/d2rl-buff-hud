# session-secrets

Bearer tokens and server keys, kept out of the mod folder.

D2RLoader hands a host's `mods/<mod>` tree, `d2rloader/config/*.toml` included,
to players who join it. A launcher token written into a plugin config therefore
ends up on other people's machines - and one did. Secrets live here instead:

| File | Written by | Keys |
|---|---|---|
| `<game folder>/reimagined-secrets/session.toml` | the launcher, before every launch; deleted when the game exits | `access_token`, `expires_at_unix`, `ladder_launch_ticket`, `status_session_id` |
| `<game folder>/reimagined-secrets/server.toml` | d2rds host provisioning (`scripts/setup-local-d2rds.ps1`) | `lobby_server_key`, `api_token` |

The game folder is the one holding the running `D2R.exe` - the install root,
outside `mods/`.

## The rule

A key the secrets file carries wins, even when its value is empty, so a token
left in an old config can never stand in for the live session. Only when the
file is missing, expired or lacks the key does the plugin's own config value
count: a developer override for running the game without the launcher, logged
as a warning.

A `session.toml` whose `expires_at_unix` is missing or in the past is ignored as
a whole. That is what keeps a file a crashed launcher never deleted from signing
a later, unrelated launch in.

## Use

```cpp
#include <D2RLSecrets/session_secrets.h>

LoadConfig(context);
g_config.accessToken = D2RLSecrets::LoadSessionSecret(
    context, "my-plugin", "access_token", g_config.accessToken, "my-plugin.toml");
```

`LoadSessionSecret` / `LoadServerSecret` log where the value came from, never the
value. `ReadSessionFile`, `Resolve` and the parsers are there for callers that
need the file itself (lobby-bridge, supporter-portals).

`IsLadderLaunch(context)` is true when the active mod is `ReimaginedLadder` or
the live `session.toml` carries a `ladder_launch_ticket`. Plugins use it at load
to switch off debug and test commands. They also use it to ignore config
overrides of settings that shape a ladder's game.

Link `D2RL::SessionSecrets`. Tests: `scripts/run-session-secrets-tests.ps1`.

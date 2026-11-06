# Garden Guard hosted outputs

A standalone Cloudflare Worker serving two outputs: the board's actual OLED
framebuffer and its UART messages.

`tools/cloud_relay.py --outputs-only` captures the selected board outputs and
uploads them over authenticated HTTPS. The cloud retains the actual receipt
times and marks stale data. The database starts empty. The existing telemetry
API remains compatible, and public mission writes remain disabled.

## Build and check

No dependency installation is required. Use Node with its built-in test
runner and SQLite support for the tests.

```sh
bash scripts/build.sh
node scripts/validate-artifact.mjs
node --test tests/*.test.mjs
```

The builder embeds `assets/index.html`, `assets/app.css` and `assets/app.js`
into `dist/server/index.js`. That deployment entry has no runtime imports.
Edit the authored files, then rebuild; do not edit generated `dist/` files.

## Runtime and publication

The active deployment is
<https://garden-guard.brian-ee2028.workers.dev/drone/GG01>.
`wrangler.jsonc` contains its account and D1 binding identifiers, without
credentials. Use [the deployment steps](CLOUDFLARE.md) to update it or copy
`wrangler.example.jsonc` when deploying to a different account. Free-plan
quotas apply to the current upload and playback traffic.

The active `wrangler.jsonc` declares the logical D1 binding `DB`. Its schema-only
migration in `drizzle/` is applied with Wrangler. The database stores one bounded
current state and output record for GG01. The old `.openai/hosting.json` and
Drizzle journal retain the original Site identity for the previous host.

Set `RELAY_TOKEN` as a secret in the Worker's runtime environment and use the
same value for the Python relay. Never add it to assets, source or URLs.
`POST /api/relay/sync` requires that token. `GET /api/state` is public;
`POST /api/mission` returns 403. The browser polls every 250 ms and plays up to
20 verified captured frames per batch through a 500 ms buffer. Stream IDs
separate capture sessions; stale/offline images freeze and final frames bypass
the buffer.

Publishing to Cloudflare uses the built entry and the checked-in Wrangler
configuration. The Sites source workflow applies only when updating the
previous host; retain its existing ID in that case.

See [the cloud hosting guide](../../docs/cloud-hosting.md) for the public URL,
gateway startup and the NFC firmware override.

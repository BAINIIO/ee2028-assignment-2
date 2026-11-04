# Garden Guard hosted command centre

A standalone Cloudflare Worker serving the existing Garden Guard dashboard
and a persistent, read-only view of real GG01 telemetry.

The firmware's native Wi-Fi TCP stream still reaches the Mac receiver.
`tools/cloud_relay.py` uploads that receiver's state over authenticated HTTPS.
The cloud retains the actual source and receipt time, and expires stale data.
The database starts empty. Public mission writes are disabled.

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

The tracked `.openai/hosting.json` preserves the registered Site ID and
declares the logical D1 binding `DB`. The schema-only migration and journal
in `drizzle/` are included by the Sites archive helper and applied on deploy.
The database stores one bounded current state/history record for GG01.

Set `RELAY_TOKEN` as a secret in the Site's runtime environment and use the
same value for the Python relay. Never add it to assets, source or URLs.
`POST /api/relay/sync` requires that token. `GET /api/state` is public;
`POST /api/mission` returns 403. The browser polls once per second.

Publishing uses the Sites source workflow with the exact pushed source
commit and a matching archive. Its isolated publishing checkout is outside
the firmware repository. Reuse this Site's existing ID when updating it.

See [the cloud hosting guide](../../docs/cloud-hosting.md) for the public URL,
gateway startup and the NFC firmware override.

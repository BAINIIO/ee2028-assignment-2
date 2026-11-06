# Deploy to workers.dev

The active website is
<https://garden-guard.brian-ee2028.workers.dev/drone/GG01>.
The checked-in `wrangler.jsonc` identifies this Worker and its `garden-guard`
D1 database. It contains no credentials. The existing Sites manifest and Site
identity remain available for the previous host.
[Cloudflare routing](https://developers.cloudflare.com/workers/configuration/routing/workers-dev/).

## Update the existing deployment

Run from this directory:

```sh
npx wrangler login --use-keyring --scopes account:read user:read workers_scripts:write d1:write
node scripts/embed-assets.mjs
node scripts/validate-artifact.mjs
node --test tests/*.test.mjs
npx wrangler deploy --config wrangler.jsonc
```

The login uses encrypted storage backed by the OS keychain. The explicit
`workers_scripts:write` scope is needed by the current Workers API; the older
`workers:write` OAuth scope alone returned 403 during this setup. Existing
secrets survive normal Worker deployments. Do not recreate the database or
rotate `RELAY_TOKEN` during routine code updates.

The initial deployment used Cloudflare's documented Scripts upload API after
Wrangler's initial service lookup returned 503. Its OAuth credential was
obtained through `wrangler auth token --json` and used only in process memory;
no authentication tokens were saved in project files.

## Deploy to another account

Run these steps from `web/command-centre-cloud/` after account access is ready.
They require Node/npm and Wrangler; `npx` may download Wrangler when first used.

1. Authenticate and create the new database:

   ```sh
   npx wrangler login --use-keyring --scopes account:read user:read workers_scripts:write d1:write
   npx wrangler d1 create garden-guard
   cp wrangler.example.jsonc wrangler.jsonc
   ```

   Replace `REPLACE_WITH_CLOUDFLARE_D1_UUID` in `wrangler.jsonc` with the returned
   database UUID. On PowerShell, use `Copy-Item` instead of `cp`. The example
   filename is deliberately not auto-selected, and its placeholder is not a
   deployable database ID. The example creates a separate deployment; preserve
   the existing checked-in configuration when continuing this deployment.
   [D1 creation](https://developers.cloudflare.com/d1/wrangler-commands/#d1-create).

2. Build and validate the exact standalone entry:

   ```sh
   node scripts/embed-assets.mjs
   node scripts/validate-artifact.mjs
   node --test tests/*.test.mjs
   npx wrangler deploy --dry-run --config wrangler.jsonc
   ```

   `dist/server/index.js` includes the OLED/UART assets. Deploying the authored
   `worker/index.js` directly would omit them. Wrangler also runs the embedding
   build automatically before deployment. Local tests require Node's SQLite
   support. The dry run bundles locally without publishing the Worker.

3. Apply the existing schema and deploy:

   ```sh
   npx wrangler d1 migrations list garden-guard --remote --config wrangler.jsonc
   npx wrangler d1 migrations apply garden-guard --remote --config wrangler.jsonc
   npx wrangler deploy --config wrangler.jsonc
   npx wrangler secret put RELAY_TOKEN --config wrangler.jsonc
   ```

   Enter a fresh private relay token at the secret prompt, then use the same
   token in the gateway's hidden prompt. Keep it out of configuration, browser
   assets and URLs. `secret put` publishes a new Worker version; set it before
   starting uploads. Until configured, authenticated ingestion returns 503.
   [Secrets](https://developers.cloudflare.com/workers/configuration/secrets/).

   The `DB` binding points to the new `garden-guard` database. `migrations_dir`
   reuses `drizzle/0000_garden_guard_state.sql` unchanged; Wrangler records applied
   SQL in its own migration table. The Sites journal stays intact and does not
   initialize this new database. Do not run a second schema import after applying
   that migration. This starts with empty outputs; existing Sites data is not
   copied. [Migrations](https://developers.cloudflare.com/d1/reference/migrations/).

4. Use the actual URL printed by Wrangler. Open `/drone/GG01` and `/api/state`:
   the page should initially show empty outputs, and public mission writes must
   remain forbidden. From the firmware project directory, point the gateway at
   that new origin, replacing the uppercase placeholders:

   ```sh
   python3 tools/cloud_relay.py --outputs-only --oled \
     --cloud-url https://garden-guard.YOUR_SUBDOMAIN.workers.dev \
     --uart-port /dev/cu.usbmodemYOUR_PORT --token-prompt
   ```

   The explicit `--cloud-url` selects the new host. For Windows or an existing
   UART log, use the capture options in [the hosting guide](../../docs/cloud-hosting.md).
   Use the new `/drone/GG01` URL for a later NFC configuration; deploying a host
   does not rewrite the board's existing tag.

Free-plan limits still apply: Workers includes 100,000 requests/day and 10 ms
CPU per request; D1 includes 5 million rows read/day, 100,000 rows written/day
and 5 GB storage. With the current 4 Hz upload and one 4 Hz browser, requests
alone reach 100,000 after roughly 3 hours 28 minutes, before other traffic.
The shared Tera Term bridge adds about two reads per second, reducing that
example to about 2 hours 47 minutes. Additional browser tabs also count.
Short demonstrations fit this model; continuous streaming can exhaust daily
quotas. [Workers limits/pricing](https://developers.cloudflare.com/workers/platform/pricing/),
[D1 pricing](https://developers.cloudflare.com/d1/platform/pricing/).

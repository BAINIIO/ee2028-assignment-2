# Hosted Garden Guard dashboard

The hosted dashboard is at
<https://garden-guard-gg01.spunkybroom4.chatgpt.site/drone/GG01>.
Anyone with the link can view it. The website runs in the cloud and opens
even when the Mac is off. Sensor readings require a connected board and a
running telemetry relay; the page shows no readings until real packets arrive.

## What runs where

```text
STM32 native Wi-Fi -> Mac receiver :8081 -> HTTPS relay -> cloud database
                                                              |
                                                              v
                                                     phone / laptop browser

STM32 USART1 -> ST-Link virtual COM port -> Tera Term / UART monitor
```

The cloud host accepts HTTPS, rather than the firmware's current raw TCP
stream. The Mac is therefore still the telemetry gateway. Hosting the page
does not fix the board's pending hotspot association or make the STM32 connect
directly to the Internet. UART output and the physical USER button continue
to work independently.

The dashboard preserves the actual transport: native board Wi-Fi is labelled
Wi-Fi; any explicitly enabled serial fallback is labelled UART. A relay
heartbeat does not count as a new sensor reading. Old packets become stale
and then offline; the last known values retain their original receipt time.
The production database starts empty, without simulated sensor data.

The public dashboard is read-only. Mission selection remains available on
the local console when a live native Wi-Fi board is in standby. Public
visitors cannot charge, rescue or change missions.

## Start live updates

Start the existing receiver from the project directory:

```sh
python3 tools/command_centre.py
```

Then start the HTTPS relay in another terminal:

```sh
python3 tools/cloud_relay.py \
  --cloud-url https://garden-guard-gg01.spunkybroom4.chatgpt.site \
  --local-url http://127.0.0.1:8080 --token-prompt
```

The relay asks for a private upload token using a hidden prompt. The matching
`RELAY_TOKEN` is a secret runtime setting on the hosted Site. It belongs only
on the gateway and server; never place it in browser JavaScript, an NFC URL,
screenshots or Git. To replace a lost token, configure the same new random
value on the Site and the relay, then redeploy the existing saved version so
the server uses the new setting.

Keep the Mac receiver reachable by the board on its configured local IPv4
address. The relay uses loopback to read that receiver and verified HTTPS to
upload the state. A reconnect resumes updates without changing the website
address. The cloud website remains available after both local processes stop.

## NFC phone access

Use this clean base URL for the next NFC firmware configuration:

```text
https://garden-guard-gg01.spunkybroom4.chatgpt.site/drone/GG01
```

For the existing firmware build configuration:

```sh
make EXTRA_DEFINES='-DAPP_NFC_DASHBOARD_URL=\"https://garden-guard-gg01.spunkybroom4.chatgpt.site/drone/GG01\"'
```

Building does not change the physical board. Review the intended firmware,
then use the existing explicit flash workflow when the board is reconnected.
The firmware appends its current `?mission=N` context and verifies the tag's
stored record. This link fits the firmware's 179-byte base-URL limit.

The tag last verified on 1 November still contains the earlier local URL.
Publishing the website does not rewrite that EEPROM. Once the tag contains
the hosted link, an iPhone can open the page using mobile data or another
Internet connection; it no longer needs to reach the Mac's local HTTP server.

## Hosting source

`web/command-centre-cloud/` contains the standalone Worker, embedded dashboard
assets, database migration and hosting manifest. The existing local dashboard
in `web/command-centre/dist/` is retained for local operation. Deployment uses
the registered Site ID from `.openai/hosting.json`; reuse that ID on updates.
The publishing checkout lives outside the firmware repository so Site source
synchronization cannot commit local firmware credentials or hardware images.

The cloud upload endpoint requires the server-held token. Browser access to
state is public, and the public mission endpoint rejects writes. Wi-Fi
passwords and firmware binaries are never needed by the hosted website.

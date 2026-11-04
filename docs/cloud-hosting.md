# Hosted OLED and UART output

The website is at
<https://garden-guard-gg01.spunkybroom4.chatgpt.site/drone/GG01>.
Anyone with the link can view it. It contains two sections: the board's
actual 128×64 OLED image and its UART text. The website runs in the cloud and
opens even when the Mac is off. Live outputs need the board and capture
relay; empty outputs stay empty until real data arrives.

## What runs where

```text
STM32 OLED framebuffer -> ST-Link read -> Mac HTTPS relay -> hosted OLED
STM32 USART1 -> ST-Link virtual COM port -> same relay -> hosted UART
```

The OLED image comes from the real framebuffer, rather than a browser
reconstruction of the game. UART lines come from serial reception or an
explicitly selected UART log, rather than generated telemetry summaries.
The page retains the last output and marks it stale or offline when capture
stops. A successful relay upload does not make an old frame or log line new.
The browser updates approximately once per second. The existing native laptop
viewer remains the option for the local 30 fps display.

The public page is read-only. The physical USER button still controls the
game. The board's direct hotspot association remains unverified; these
captured outputs use the Mac's Internet connection.

## Start live updates

Connect the board's ST-Link USB port and run from the project directory:

```sh
python3 tools/cloud_relay.py --outputs-only --oled \
  --uart-port /dev/cu.usbmodemYOUR_PORT --token-prompt
```

Replace the serial device with the board's actual virtual COM port. This mode
does not need the local command-centre server. It reads the OLED framebuffer
without resetting or flashing the board. Its ELF/BIN verification requires
the existing `build/firmware/` image to match the firmware on the board; a
mismatch leaves capture offline with an explanatory console error.

If another terminal already receives UART, select its live log instead of
opening the serial port again:

```sh
python3 tools/cloud_relay.py --outputs-only --oled \
  --uart-log /path/to/live-uart.log --token-prompt
```

The default destination is the hosted website above. The relay asks for a
private upload token using a hidden prompt. The matching
`RELAY_TOKEN` is a secret runtime setting on the hosted Site. It belongs only
on the gateway and server; never place it in browser JavaScript, an NFC URL,
screenshots or Git. To replace a lost token, configure the same new random
value on the Site and the relay, then redeploy the existing saved version so
the server uses the new setting.

The relay uses verified HTTPS and keeps at most 100 UART lines in its upload.
The website address stays the same when the Mac's local IP changes. The cloud
website remains available after capture stops. When the board terminates, its
last outputs remain marked ended. Reset the board and restart capture to begin
a new session.

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

`web/command-centre-cloud/` contains the standalone Worker, embedded output page,
assets, database migration and hosting manifest. The existing local dashboard
in `web/command-centre/dist/` is retained for local operation. Deployment uses
the registered Site ID from `.openai/hosting.json`; reuse that ID on updates.
The publishing checkout lives outside the firmware repository so Site source
synchronization cannot commit local firmware credentials or hardware images.

The cloud upload endpoint requires the server-held token. Browser access to
state is public, and the public mission endpoint rejects writes. Wi-Fi
passwords and firmware binaries are never needed by the hosted website.

#!/usr/bin/env node
"use strict";

const fs = require("fs");
const http = require("http");
const path = require("path");

function usage() {
  console.log(`Usage: node run-webrtc.js --ws-url URL [options]

Options:
  --ws-url URL       WebSocket URL (or CS_WS_URL)
  --base-url URL     Base URL to derive WS URL (or CS_BASE_URL)
  --stun URL         STUN server URL (or CS_STUN_SERVER)
  --timeout-ms N     Timeout in ms (or CS_TIMEOUT_MS, default 20000)
  --min-frames N     Frames required (or CS_MIN_FRAMES, default 3)
  --headed           Run with visible browser window
  --insecure         Ignore TLS errors
  --verbose          Emit console logs
  --help             Show this help
`);
}

function loadPlaywright() {
  try {
    return require("playwright").chromium;
  } catch (err) {
    console.error("Playwright is not installed. Run: npm install && npm run install-browsers");
    process.exit(1);
  }
}

const args = process.argv.slice(2);
const opts = {
  wsUrl: process.env.CS_WS_URL || "",
  baseUrl: process.env.CS_BASE_URL || "",
  stun: process.env.CS_STUN_SERVER || "",
  timeoutMs: Number(process.env.CS_TIMEOUT_MS) || 20000,
  minFrames: Number(process.env.CS_MIN_FRAMES) || 3,
  headed: false,
  insecure: false,
  verbose: false,
};

for (let i = 0; i < args.length; i += 1) {
  const arg = args[i];
  if (arg === "--ws-url") {
    opts.wsUrl = args[i + 1];
    i += 1;
  } else if (arg === "--base-url") {
    opts.baseUrl = args[i + 1];
    i += 1;
  } else if (arg === "--stun") {
    opts.stun = args[i + 1];
    i += 1;
  } else if (arg === "--timeout-ms") {
    opts.timeoutMs = Number(args[i + 1]);
    i += 1;
  } else if (arg === "--min-frames") {
    opts.minFrames = Number(args[i + 1]);
    i += 1;
  } else if (arg === "--headed") {
    opts.headed = true;
  } else if (arg === "--insecure") {
    opts.insecure = true;
  } else if (arg === "--verbose") {
    opts.verbose = true;
  } else if (arg === "--help") {
    usage();
    process.exit(0);
  } else {
    console.error(`Unknown flag: ${arg}`);
    usage();
    process.exit(1);
  }
}

function deriveWsUrl(baseUrl) {
  const trimmed = baseUrl.replace(/\/$/, "");
  if (trimmed.startsWith("http://")) {
    return `ws://${trimmed.slice("http://".length)}/ws`;
  }
  if (trimmed.startsWith("https://")) {
    return `wss://${trimmed.slice("https://".length)}/ws`;
  }
  return "";
}

if (!opts.wsUrl && opts.baseUrl) {
  opts.wsUrl = deriveWsUrl(opts.baseUrl);
}

if (!opts.wsUrl) {
  console.error("Missing --ws-url (or CS_WS_URL). Provide --base-url to derive.");
  usage();
  process.exit(1);
}

function serveStatic(rootDir) {
  return http.createServer((req, res) => {
    const urlPath = (req.url || "/").split("?")[0];
    const safePath = path.normalize(urlPath === "/" ? "/rtcp_probe.html" : urlPath);
    const resolved = path.join(rootDir, safePath);

    if (!resolved.startsWith(rootDir)) {
      res.statusCode = 403;
      res.end("Forbidden");
      return;
    }

    fs.readFile(resolved, (err, data) => {
      if (err) {
        res.statusCode = 404;
        res.end("Not found");
        return;
      }
      if (resolved.endsWith(".html")) {
        res.setHeader("Content-Type", "text/html; charset=utf-8");
      }
      res.end(data);
    });
  });
}

async function run() {
  const chromium = loadPlaywright();
  const rootDir = __dirname;
  const server = serveStatic(rootDir);

  await new Promise((resolve) => {
    server.listen(0, "127.0.0.1", resolve);
  });

  const address = server.address();
  const port = typeof address === "string" ? 0 : address.port;

  const params = new URLSearchParams();
  params.set("ws", opts.wsUrl);
  if (opts.stun) {
    params.set("stun", opts.stun);
  }

  const probeUrl = `http://127.0.0.1:${port}/rtcp_probe.html?${params.toString()}`;

  const launchArgs = ["--autoplay-policy=no-user-gesture-required"];
  if (opts.insecure) {
    launchArgs.push("--ignore-certificate-errors");
  }

  const browser = await chromium.launch({
    headless: !opts.headed,
    args: launchArgs,
  });

  const context = await browser.newContext({
    ignoreHTTPSErrors: opts.insecure,
  });

  const page = await context.newPage();
  if (opts.verbose) {
    page.on("console", (msg) => {
      console.log(`[browser:${msg.type()}] ${msg.text()}`);
    });
  }

  page.on("pageerror", (err) => {
    console.error(`[browser:error] ${err.message}`);
  });

  try {
    await page.goto(probeUrl, { waitUntil: "load", timeout: opts.timeoutMs });
    const result = await page.evaluate(
      async ({ timeoutMs, minFrames }) => {
        return window.__e2eWaitForFrames(timeoutMs, minFrames);
      },
      { timeoutMs: opts.timeoutMs, minFrames: opts.minFrames }
    );

    console.log("E2E OK");
    console.log(JSON.stringify(result, null, 2));
  } catch (err) {
    const status = await page.evaluate(() => window.__e2eStatus || null).catch(() => null);
    console.error("E2E FAILED");
    if (status) {
      console.error(JSON.stringify(status, null, 2));
    }
    throw err;
  } finally {
    await Promise.allSettled([context.close(), browser.close()]);
    server.close();
  }
}

run().catch((err) => {
  console.error(err.message || err);
  process.exit(1);
});

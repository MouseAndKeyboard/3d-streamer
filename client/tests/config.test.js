import assert from "node:assert/strict";
import { test } from "node:test";

import {
  defaultWsUrl,
  formatIceServers,
  parseBoolean,
  parseIceServers
} from "../src/config.js";

test("parseBoolean uses fallback when value is empty", () => {
  assert.equal(parseBoolean(null, "true"), true);
  assert.equal(parseBoolean(undefined, "0"), false);
});

test("parseBoolean parses explicit values", () => {
  assert.equal(parseBoolean("true", false), true);
  assert.equal(parseBoolean("1", false), true);
  assert.equal(parseBoolean("false", true), false);
});

test("parseIceServers splits and trims entries", () => {
  assert.deepEqual(parseIceServers(""), []);
  assert.deepEqual(parseIceServers("stun:one, turn:two"), [
    { urls: "stun:one" },
    { urls: "turn:two" }
  ]);
});

test("formatIceServers renders readable label", () => {
  assert.equal(formatIceServers([]), "none");
  assert.equal(formatIceServers([{ urls: "stun:one" }]), "stun:one");
});

test("defaultWsUrl picks localhost port for local dev", () => {
  const url = defaultWsUrl({
    protocol: "http:",
    hostname: "localhost",
    host: "localhost:5173"
  });
  assert.equal(url, "ws://localhost:8080");
});

test("defaultWsUrl uses /ws for non-local hosts", () => {
  const url = defaultWsUrl({
    protocol: "https:",
    hostname: "stream.example.com",
    host: "stream.example.com"
  });
  assert.equal(url, "wss://stream.example.com/ws");
});

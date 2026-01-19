const FALLBACK_LOCATION = {
  protocol: "http:",
  hostname: "localhost",
  host: "localhost"
};

export function parseBoolean(value, fallback) {
  if (value === null || value === undefined || value === "") {
    return String(fallback) === "true" || String(fallback) === "1";
  }
  return value === "true" || value === "1";
}

export function parseIceServers(value) {
  if (!value) {
    return [];
  }
  return value
    .split(",")
    .map((entry) => entry.trim())
    .filter(Boolean)
    .map((entry) => ({ urls: entry }));
}

export function formatIceServers(servers) {
  if (!servers.length) {
    return "none";
  }
  return servers
    .map((server) => {
      if (Array.isArray(server.urls)) {
        return server.urls.join(",");
      }
      return server.urls;
    })
    .join(" | ");
}

export function defaultWsUrl(locationValue) {
  const location =
    locationValue ||
    (typeof window === "undefined" ? FALLBACK_LOCATION : window.location);
  const protocol = location.protocol === "https:" ? "wss" : "ws";
  const hostname = location.hostname || "localhost";
  const isLocal =
    hostname === "localhost" ||
    hostname === "127.0.0.1" ||
    hostname.endsWith(".local");
  if (isLocal) {
    return `${protocol}://${hostname}:8080`;
  }
  return `${protocol}://${location.host}/ws`;
}

import {
  defaultWsUrl,
  formatIceServers,
  parseBoolean,
  parseIceServers
} from "./config.js";

const statusEl = document.getElementById("status");
const connectBtn = document.getElementById("connect-btn");
const detailsEl = document.getElementById("details");
const videoEl = document.getElementById("video");

const params = new URLSearchParams(window.location.search);

const signalingUrl =
  params.get("ws") || import.meta.env.VITE_SIGNALING_URL || defaultWsUrl();

const iceServers = parseIceServers(
  params.get("ice") || import.meta.env.VITE_ICE_SERVERS
);

const debug = parseBoolean(
  params.get("debug"),
  import.meta.env.VITE_DEBUG
);

const autoConnect = parseBoolean(
  params.get("auto"),
  import.meta.env.VITE_AUTO_CONNECT
);

const baseDetailLines = [
  `Signaling: ${signalingUrl}`,
  `ICE: ${formatIceServers(iceServers)}`
];

renderDetails();

const receiver = new Receiver({ signalingUrl, iceServers, debug });

connectBtn.addEventListener("click", () => {
  if (receiver.isActive()) {
    receiver.disconnect();
    return;
  }
  receiver.connect();
});

if (autoConnect) {
  receiver.connect();
}

function setStatus(label, tone) {
  statusEl.textContent = label;
  statusEl.className = `status ${tone}`;
}

function setButtonState(isActive) {
  connectBtn.textContent = isActive ? "Disconnect" : "Connect";
}

function renderDetails(extraLine = "") {
  const lines = [...baseDetailLines];
  if (debug) {
    lines.push("Debug: on");
  }
  if (extraLine) {
    lines.push(extraLine);
  }
  detailsEl.textContent = lines.join("\n");
}

function Receiver({ signalingUrl, iceServers, debug }) {
  this.signalingUrl = signalingUrl;
  this.iceServers = iceServers;
  this.debug = debug;
  this.ws = null;
  this.pc = null;
  this.pendingCandidates = [];
  this.reconnectTimer = null;
  this.reconnectAttempt = 0;
  this.manualDisconnect = false;
  this.statsTimer = null;
  this.lastStats = null;
  this.iceRestarted = false;
}

Receiver.prototype.isActive = function isActive() {
  return Boolean(this.ws || this.pc);
};

Receiver.prototype.connect = function connect() {
  if (this.ws || this.pc) {
    return;
  }
  this.manualDisconnect = false;
  setButtonState(true);
  setStatus("Connecting", "connecting");
  this.openWebSocket();
};

Receiver.prototype.disconnect = function disconnect() {
  this.manualDisconnect = true;
  this.clearReconnect();
  this.teardown();
  setButtonState(false);
  setStatus("Idle", "idle");
  renderDetails();
};

Receiver.prototype.openWebSocket = function openWebSocket() {
  const ws = new WebSocket(this.signalingUrl);
  this.ws = ws;

  ws.addEventListener("open", () => {
    if (this.ws !== ws) {
      return;
    }
    this.reconnectAttempt = 0;
    setStatus("Negotiating", "connecting");
    this.ensurePeerConnection();
    this.send({ type: "hello", role: "viewer" });
    this.send({ type: "ready" });
  });

  ws.addEventListener("message", (event) => {
    if (this.ws !== ws) {
      return;
    }
    this.handleMessage(event);
  });

  ws.addEventListener("close", () => {
    if (this.ws !== ws) {
      return;
    }
    this.ws = null;
    if (!this.manualDisconnect) {
      this.scheduleReconnect("signaling closed");
    }
  });

  ws.addEventListener("error", () => {
    if (this.ws !== ws) {
      return;
    }
    setStatus("Signaling error", "error");
  });
};

Receiver.prototype.handleMessage = async function handleMessage(event) {
  let message;
  try {
    message = JSON.parse(event.data);
  } catch (error) {
    this.log("Ignoring malformed message.");
    return;
  }

  if (!message || typeof message.type !== "string") {
    return;
  }

  switch (message.type) {
    case "offer":
      await this.handleOffer(message);
      break;
    case "answer":
      await this.handleAnswer(message);
      break;
    case "ice":
      this.handleRemoteIce(message);
      break;
    case "bye":
      this.handleBye(message);
      break;
    case "error":
      setStatus("Server error", "error");
      if (message.reason) {
        renderDetails(`Server error: ${message.reason}`);
      }
      break;
    default:
      this.log(`Unknown message: ${message.type}`);
      break;
  }
};

Receiver.prototype.handleOffer = async function handleOffer(message) {
  if (!message.sdp) {
    return;
  }
  this.ensurePeerConnection();
  setStatus("Answering offer", "connecting");
  try {
    await this.pc.setRemoteDescription({ type: "offer", sdp: message.sdp });
    const answer = await this.pc.createAnswer();
    await this.pc.setLocalDescription(answer);
    this.send({ type: "answer", sdp: answer.sdp });
    this.flushPendingCandidates();
  } catch (error) {
    this.log("Failed to process offer.");
    setStatus("Negotiation failed", "error");
    this.scheduleReconnect("offer failed");
  }
};

Receiver.prototype.handleAnswer = async function handleAnswer(message) {
  if (!message.sdp || !this.pc) {
    return;
  }
  try {
    await this.pc.setRemoteDescription({ type: "answer", sdp: message.sdp });
    this.flushPendingCandidates();
  } catch (error) {
    this.log("Failed to process answer.");
    setStatus("Negotiation failed", "error");
    this.scheduleReconnect("answer failed");
  }
};

Receiver.prototype.handleRemoteIce = function handleRemoteIce(message) {
  if (!message.candidate) {
    return;
  }
  const candidate = {
    candidate: message.candidate,
    sdpMid: message.sdpMid,
    sdpMLineIndex: message.sdpMLineIndex
  };

  if (!this.pc || !this.pc.remoteDescription) {
    this.pendingCandidates.push(candidate);
    return;
  }

  this.pc.addIceCandidate(new RTCIceCandidate(candidate)).catch(() => {
    this.log("Failed to add ICE candidate.");
  });
};

Receiver.prototype.handleBye = function handleBye(message) {
  const reason = message.reason ? `: ${message.reason}` : "";
  setStatus("Server closed" + reason, "error");
  this.scheduleReconnect("server closed");
};

Receiver.prototype.flushPendingCandidates = function flushPendingCandidates() {
  if (!this.pc || !this.pendingCandidates.length) {
    return;
  }
  const pending = this.pendingCandidates;
  this.pendingCandidates = [];
  pending.forEach((candidate) => {
    this.pc.addIceCandidate(new RTCIceCandidate(candidate)).catch(() => {
      this.log("Failed to add queued ICE candidate.");
    });
  });
};

Receiver.prototype.ensurePeerConnection = function ensurePeerConnection() {
  if (this.pc) {
    return;
  }

  const pc = new RTCPeerConnection({ iceServers: this.iceServers });
  this.pc = pc;
  this.iceRestarted = false;

  pc.addTransceiver("video", { direction: "recvonly" });

  pc.addEventListener("track", (event) => {
    const stream = event.streams[0];
    if (stream) {
      videoEl.srcObject = stream;
      videoEl.play().catch(() => {});
    }
  });

  pc.addEventListener("icecandidate", (event) => {
    if (event.candidate) {
      this.send({
        type: "ice",
        candidate: event.candidate.candidate,
        sdpMid: event.candidate.sdpMid,
        sdpMLineIndex: event.candidate.sdpMLineIndex
      });
    }
  });

  pc.addEventListener("connectionstatechange", () => {
    if (!this.pc) {
      return;
    }
    const state = this.pc.connectionState;
    if (state === "connected") {
      setStatus("Streaming", "streaming");
      setButtonState(true);
      this.iceRestarted = false;
      this.startStats();
      return;
    }
    if (state === "failed") {
      if (!this.iceRestarted && typeof this.pc.restartIce === "function") {
        this.iceRestarted = true;
        setStatus("Restarting ICE", "connecting");
        this.pc.restartIce();
        return;
      }
      setStatus("Connection failed", "error");
      this.scheduleReconnect("connection failed");
    }
  });

  pc.addEventListener("iceconnectionstatechange", () => {
    if (!this.pc) {
      return;
    }
    if (this.pc.iceConnectionState === "failed") {
      if (!this.iceRestarted && typeof this.pc.restartIce === "function") {
        this.iceRestarted = true;
        setStatus("Restarting ICE", "connecting");
        this.pc.restartIce();
        return;
      }
      setStatus("ICE failed", "error");
      this.scheduleReconnect("ice failed");
    }
  });
};

Receiver.prototype.send = function send(message) {
  if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
    return;
  }
  this.ws.send(JSON.stringify(message));
};

Receiver.prototype.scheduleReconnect = function scheduleReconnect(reason) {
  if (this.manualDisconnect) {
    return;
  }
  if (this.reconnectTimer) {
    return;
  }
  this.teardown();
  setStatus("Reconnecting", "connecting");
  const delay = Math.min(1000 * 2 ** this.reconnectAttempt, 15000);
  this.reconnectAttempt += 1;
  renderDetails(`Retrying in ${Math.round(delay / 1000)}s (${reason})`);
  this.reconnectTimer = window.setTimeout(() => {
    this.reconnectTimer = null;
    this.connect();
  }, delay);
};

Receiver.prototype.clearReconnect = function clearReconnect() {
  if (this.reconnectTimer) {
    clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
  }
};

Receiver.prototype.teardown = function teardown() {
  if (this.ws) {
    this.ws.close();
    this.ws = null;
  }
  if (this.pc) {
    this.pc.close();
    this.pc = null;
  }
  this.pendingCandidates = [];
  if (videoEl.srcObject) {
    videoEl.srcObject = null;
  }
  this.stopStats();
};

Receiver.prototype.startStats = function startStats() {
  if (!this.debug || this.statsTimer || !this.pc) {
    return;
  }

  this.statsTimer = window.setInterval(async () => {
    if (!this.pc) {
      return;
    }
    const stats = await this.pc.getStats();
    let inbound;
    stats.forEach((report) => {
      const kind = report.kind || report.mediaType;
      if (report.type === "inbound-rtp" && kind === "video") {
        inbound = report;
      }
    });
    if (!inbound) {
      return;
    }

    const now = inbound.timestamp;
    const bytes = inbound.bytesReceived;
    let bitrateKbps = 0;
    if (this.lastStats) {
      const deltaTime = (now - this.lastStats.timestamp) / 1000;
      const deltaBytes = bytes - this.lastStats.bytesReceived;
      if (deltaTime > 0) {
        bitrateKbps = (deltaBytes * 8) / 1000 / deltaTime;
      }
    }
    this.lastStats = { timestamp: now, bytesReceived: bytes };
    renderDetails(`Bitrate: ${Math.round(bitrateKbps)} kbps`);
  }, 1000);
};

Receiver.prototype.stopStats = function stopStats() {
  if (this.statsTimer) {
    clearInterval(this.statsTimer);
    this.statsTimer = null;
  }
  this.lastStats = null;
};

Receiver.prototype.log = function log(message) {
  if (!this.debug) {
    return;
  }
  console.log(`[receiver] ${message}`);
};

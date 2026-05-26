const apiBase = location.port === "8000" ? "" : `${location.protocol}//${location.hostname}:8000`;

const $ = (id) => document.getElementById(id);

function log(message, detail) {
  const line = `[${new Date().toLocaleTimeString()}] ${message}`;
  const current = $("log").textContent;
  $("log").textContent = detail ? `${line}\n${detail}\n\n${current}` : `${line}\n\n${current}`;
}

async function request(path, options = {}) {
  const response = await fetch(`${apiBase}${path}`, {
    headers: { "Content-Type": "application/json" },
    ...options,
  });
  const text = await response.text();
  let payload = {};
  if (text.trim()) {
    try {
      payload = JSON.parse(text);
    } catch {
      payload = { raw: text };
    }
  }
  if (!response.ok) {
    throw new Error(payload.error || `${response.status} ${response.statusText}`);
  }
  return payload;
}

function numberValue(id) {
  const value = $(id).value;
  return value === "" ? undefined : Number(value);
}

function fillRte(data) {
  $("rte-enabled").checked = Boolean(data.enabled);
  $("delay-offset").value = data.delay_offset ?? "";
  $("fcw").value = data.fcw ?? "";
  $("scaling").value = data.scaling ?? "";
  $("phase-offset").value = data.phase_offset ?? "";
  $("rte-timestamp").textContent = data.timestamp ?? "-";
  $("scaling-raw").textContent = data.scaling_raw ?? "-";
  $("rte-device").textContent = data.device ?? "-";
}

function fillAd9361(data) {
  $("rx-lo").value = data.rx_lo_frequency ?? "";
  $("tx-lo").value = data.tx_lo_frequency ?? "";
  $("sampling-frequency").value = data.sampling_frequency ?? "";
  $("rx-rf-bandwidth").value = data.rx_rf_bandwidth ?? "";
  $("tx-rf-bandwidth").value = data.tx_rf_bandwidth ?? "";
  $("rx-gain-mode").value = data.rx_gain_mode ?? "slow_attack";
  $("rx-gain").value = data.rx_gain ?? "";
  $("tx-gain").value = data.tx_gain ?? "";
  $("ad9361-device").textContent = data.device ?? "-";
}

async function refresh() {
  $("connection").textContent = "Refreshing";
  const [rte, ad9361] = await Promise.all([
    request("/api/rte"),
    request("/api/ad9361"),
  ]);
  fillRte(rte);
  fillAd9361(ad9361);
  $("connection").textContent = "Connected";
  log("Refreshed device state");
}

async function applyRte(event) {
  event.preventDefault();
  const payload = {
    enabled: $("rte-enabled").checked,
    delay_offset: numberValue("delay-offset"),
    fcw: numberValue("fcw"),
    scaling: numberValue("scaling"),
    phase_offset: numberValue("phase-offset"),
  };
  Object.keys(payload).forEach((key) => payload[key] === undefined && delete payload[key]);
  const data = await request("/api/rte", {
    method: "PATCH",
    body: JSON.stringify(payload),
  });
  fillRte(data);
  log("Applied RTE parameters", JSON.stringify(payload));
}

async function applyAd9361(event) {
  event.preventDefault();
  const payload = {
    rx_lo_frequency: numberValue("rx-lo"),
    tx_lo_frequency: numberValue("tx-lo"),
    sampling_frequency: numberValue("sampling-frequency"),
    rx_rf_bandwidth: numberValue("rx-rf-bandwidth"),
    tx_rf_bandwidth: numberValue("tx-rf-bandwidth"),
    rx_gain_mode: $("rx-gain-mode").value,
    rx_gain: numberValue("rx-gain"),
    tx_gain: numberValue("tx-gain"),
  };
  Object.keys(payload).forEach((key) => payload[key] === undefined && delete payload[key]);
  const data = await request("/api/ad9361", {
    method: "PATCH",
    body: JSON.stringify(payload),
  });
  fillAd9361(data);
  log("Applied AD9361 parameters", JSON.stringify(payload));
}

async function pulse(path, message) {
  await request(path, { method: "POST", body: "{}" });
  log(message);
  await refresh();
}

function wireUi() {
  $("refresh").addEventListener("click", () => refresh().catch(showError));
  $("rte-form").addEventListener("submit", (event) => applyRte(event).catch(showError));
  $("ad9361-form").addEventListener("submit", (event) => applyAd9361(event).catch(showError));
  $("reset-rte").addEventListener("click", () => pulse("/api/rte/reset", "Pulsed RTE reset").catch(showError));
  $("load-param").addEventListener("click", () => pulse("/api/rte/load-param", "Pulsed load_param").catch(showError));
}

function showError(error) {
  $("connection").textContent = "Error";
  log("Request failed", error.message);
}

wireUi();
refresh().catch(showError);

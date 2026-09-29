"use strict";

/* Multi-target GUI, 2026-09-28. Each submit has exactly one hardware owner. */
function createRteApp(doc, fetcher, clock = globalThis) {
  const TIMEOUT_MS = 8000;
  const POLL_MS = 5000;
  const targetIds = [1, 2, 3, 4];
  const targetFields = {range_m: "range-m", radial_velocity_mps: "velocity-mps",
    gain_linear: "gain-linear", phase_offset_deg: "phase-deg"};
  const rfFields = {carrier_hz: "rf-carrier", bandwidth_hz: "rf-bandwidth",
    tx_gain_db: "rf-tx-gain", rx_gain_db: "rf-rx-gain"};
  const byId = id => doc.getElementById(id);
  const targetElement = (id, name) => byId(`target-${id}-${name}`);
  const targetInputs = id => Object.fromEntries(Object.entries(targetFields)
    .map(([key, name]) => [key, `target-${id}-${name}`]));
  const text = (id, value) => { byId(id).textContent = value; };
  const state = {snapshot: null, etag: null, busy: false,
    refreshing: false, synced: false, targetDrafts: {}, rfDirty: false,
    rfCaps: null, targetCaps: null, interval: null};
  const fmt = (number, unit = "") => Number.isFinite(number)
    ? `${Number(number.toPrecision(9))}${unit ? ` ${unit}` : ""}` : "—";
  const mhz = n => Number.isFinite(n) ? `${Number((n / 1e6).toFixed(6))} MHz` : "—";
  const target = id => state.snapshot?.targets.find(item => item.id === id);

  function defaultConfig() {
    return {enabled: false, range_m: state.targetCaps?.min_range_m ?? "",
      radial_velocity_mps: 0, gain_linear: 0, phase_offset_deg: 0};
  }
  function configDraft(config, source = "default") {
    const result = {enabled: Boolean(config.enabled), dirty: false, source};
    Object.keys(targetFields).forEach(key => { result[key] = String(config[key]); });
    return result;
  }
  function draftFromTarget(item) {
    return configDraft(item?.requested || item?.saved || defaultConfig(),
      item?.requested ? "applied" : item?.saved ? "saved" : "default");
  }
  function captureTarget(id) {
    const draft = state.targetDrafts[id];
    if (!draft) return;
    draft.enabled = targetElement(id, "enabled").checked;
    Object.entries(targetFields).forEach(([key, name]) => { draft[key] = targetElement(id, name).value; });
    draft.dirty = true;
    targetElement(id, "status").textContent = "";
    renderDraftStatus(id);
  }
  function renderDraftStatus(id) {
    const draft = state.targetDrafts[id];
    const item = target(id);
    const parts = [];
    if (draft?.dirty) parts.push("미적용 편집값이 있습니다.");
    else if (draft?.source === "saved") parts.push("저장된 편집값입니다. 아직 FPGA에 적용되지 않았습니다.");
    else if (!item?.has_config) parts.push("이번 서버 실행에서 확인된 적용값이 없습니다.");
    else parts.push("마지막 성공 입력을 표시합니다.");
    if (item?.needs_reapply) parts.push("중심 주파수가 변경되어 loadParam 재적용이 필요합니다.");
    targetElement(id, "draft-status").textContent = parts.join(" ");
    targetElement(id, "panel").dataset.dirty = String(Boolean(draft?.dirty));
    const badge = targetElement(id, "state");
    badge.textContent = !item?.hardware_state_known ? "미확인" : item.needs_reapply ? "재적용 필요"
      : item.applied?.enabled ? "활성" : "비활성";
    badge.dataset.tone = !item?.hardware_state_known ? "idle" : item.needs_reapply ? "warning" : "ok";
  }
  function renderTargetForm(id) {
    const draft = state.targetDrafts[id];
    if (draft) {
      targetElement(id, "enabled").checked = draft.enabled;
      Object.entries(targetFields).forEach(([key, name]) => { targetElement(id, name).value = draft[key]; });
    }
    renderDraftStatus(id);
  }
  function renderRfForm() {
    if (!state.snapshot || state.rfDirty) return;
    Object.entries(rfFields).forEach(([key, id]) => {
      let value = state.snapshot.rf[key];
      const requested = state.snapshot.requested_rf?.[key];
      if (key === "carrier_hz" && Number.isFinite(requested) &&
          requested >= state.rfCaps?.carrier_hz.min && requested <= state.rfCaps?.carrier_hz.max &&
          Math.abs(requested - value) <= (state.rfCaps?.carrier_hz.readback_tolerance_hz ?? 0)) {
        value = requested;
      }
      byId(id).value = String(value / (key.endsWith("_hz") ? 1e6 : 1));
    });
  }
  function lockControls() {
    const locked = state.busy || state.refreshing;
    const cannotApply = locked || !state.synced || !state.etag || state.snapshot?.degraded;
    targetIds.forEach(id => {
      [...Object.values(targetFields), "enabled"].forEach(name => { targetElement(id, name).disabled = locked; });
      targetElement(id, "apply").disabled = Boolean(cannotApply);
      targetElement(id, "revert").disabled = locked || !state.snapshot;
    });
    Object.values(rfFields).forEach(id => { byId(id).disabled = locked; });
    byId("rf-apply").disabled = Boolean(cannotApply);
    byId("rf-revert").disabled = locked || !state.snapshot;
    byId("refresh-button").disabled = locked;
  }
  function renderRows(id, rows) {
    const body = byId(id);
    body.replaceChildren();
    rows.forEach(({values}) => {
      const row = doc.createElement("tr");
      values.forEach(value => {
        const cell = doc.createElement("td");
        cell.textContent = String(value);
        row.appendChild(cell);
      });
      body.appendChild(row);
    });
  }
  function renderSnapshot() {
    const snap = state.snapshot;
    if (!snap) return;
    const rf = snap.rf;
    text("revision-pill", `REV ${snap.revision}`);
    text("actual-sample-rate", `${fmt(rf.sample_rate_hz / 1e6, "MS/s")} (고정)`);
    text("actual-lo", `${mhz(rf.rx_lo_hz)} / ${mhz(rf.tx_lo_hz)}`);
    text("actual-bandwidth", `${mhz(rf.rx_bandwidth_hz)} / ${mhz(rf.tx_bandwidth_hz)}`);
    text("actual-gains", `${fmt(rf.tx_gain_db, "dB")} / ${fmt(rf.rx_gain_db, "dB")}`);
    text("gain-mode", rf.gain_control_mode);
    text("compatible", rf.compatible);
    text("hardware-build", snap.hardware_build_id);
    text("hardware-check", fmt(snap.last_hardware_check_ms, "ms"));
    byId("degraded-banner").hidden = !snap.degraded;
    const warnings = [snap.warning];
    if (snap.targets.some(item => item.needs_reapply)) warnings.push("재적용이 필요한 표적의 패널에서 loadParam을 직접 적용하세요.");
    text("warning-message", warnings.filter(Boolean).join(" "));
    byId("warning-banner").hidden = !warnings.some(Boolean);
    renderRows("targets-body", snap.targets.map(item => {
      const applied = item.applied;
      let status = !item.hardware_state_known ? "미확인" : item.needs_reapply ? "재적용 필요" : "적용 확인";
      if (item.saved && !item.has_config) status += " · 저장 편집값 있음";
      if (applied) status += applied.enabled ? " · 활성" : " · 비활성";
      return {values: [item.id, status,
        applied ? `${fmt(applied.delay_range_m, "m")} / ${fmt(applied.delay_samples, "sample")}` : "—",
        fmt(applied?.doppler_hz, "Hz"), fmt(applied?.effective_radial_velocity_mps, "m/s"),
        fmt(applied?.linear_gain), item.has_config ? mhz(item.encoded_carrier_hz) : "—"]};
    }));
    const registers = [];
    snap.targets.forEach(item => {
      Object.entries(item.applied?.registers || {}).forEach(([name, reg]) => {
        registers.push({values: [item.id, name, reg.offset, reg.value]});
      });
      if (item.load_param && typeof item.load_param === "object") {
        registers.push({values: [item.id, "LP", item.load_param.offset, item.load_param.value]});
      }
    });
    renderRows("registers-body", registers.length ? registers : [{values: ["—", "적용 기록 없음", "—", "—"]}]);
    renderRfForm();
    targetIds.forEach(renderTargetForm);
  }
  function acceptSnapshot(response) {
    const snap = response.data;
    if (snap.hardware_profile !== "multitarget-v2" || !Array.isArray(snap.targets) ||
        snap.targets.length !== 4 || new Set(snap.targets.map(item => item.id)).size !== 4 ||
        snap.targets.some(item => ![1, 2, 3, 4].includes(item.id))) {
      throw new Error("서버의 하드웨어 프로파일 또는 표적 목록이 올바르지 않습니다.");
    }
    state.snapshot = snap;
    state.etag = response.etag;
    snap.targets.forEach(item => {
      if (!state.targetDrafts[item.id]?.dirty) state.targetDrafts[item.id] = draftFromTarget(item);
    });
    renderSnapshot();
  }
  function limits(id, min, max, step) {
    const field = byId(id);
    field.min = String(min);
    field.max = String(max);
    if (step !== undefined) field.step = String(step);
  }
  function acceptCapabilities(rf, rte) {
    state.rfCaps = rf;
    state.targetCaps = rte;
    Object.entries(rfFields).forEach(([key, id]) => {
      const divisor = key.endsWith("_hz") ? 1e6 : 1;
      const cap = rf[key];
      limits(id, cap.min / divisor, cap.max / divisor, cap.step / divisor);
    });
    targetIds.forEach(id => {
      limits(`target-${id}-range-m`, rte.min_range_m, rte.max_range_m);
      limits(`target-${id}-velocity-mps`, -rte.max_abs_velocity_mps, rte.max_abs_velocity_mps);
      limits(`target-${id}-gain-linear`, 0, rte.max_gain_linear);
    });
    text("range-limits", `${fmt(rte.min_range_m)} – ${fmt(rte.max_range_m, "m")}`);
    text("range-resolution", fmt(rte.range_resolution_m, "m"));
    text("velocity-limit", fmt(rte.max_abs_velocity_mps, "m/s"));
    text("gain-limit", fmt(rte.max_gain_linear));
    text("range-reference", rte.range_reference);
    text("rf-calibration", rte.rf_calibrated ? "보정됨" : "미보정 (DUT 디지털 입출력 기준)");
  }
  async function request(url, method = "GET", body) {
    const abort = new AbortController();
    const timer = clock.setTimeout(() => abort.abort(), TIMEOUT_MS);
    try {
      const headers = {Accept: "application/json"};
      if (body !== undefined) {
        headers["Content-Type"] = "application/json";
        headers["If-Match"] = state.etag;
      }
      const response = await fetcher(url, {method, headers, signal: abort.signal,
        cache: "no-store", ...(body !== undefined ? {body: JSON.stringify(body)} : {})});
      const data = await response.json();
      if (!response.ok) {
        const error = new Error(data.error?.message || `HTTP ${response.status}`);
        error.status = response.status;
        throw error;
      }
      return {data, etag: response.headers.get("ETag")};
    } catch (error) {
      if (error.name === "AbortError") throw new Error("요청 시간이 초과되었습니다. 적용 여부를 재조회합니다. 자동 재전송하지 않습니다.");
      throw error;
    } finally {
      clock.clearTimeout(timer);
    }
  }
  function connection(ok) {
    state.synced = ok;
    text("connection-text", ok ? "상태 조회 완료" : "재조회 필요");
    byId("connection-pill").dataset.tone = ok ? "ok" : "danger";
  }
  function showError(error) {
    text("error-message", error.status === 409
      ? `다른 요청이 먼저 적용되었습니다. ${state.synced ? "최신 상태를 조회했습니다. 편집값을 확인하고 다시 적용하세요." : "최신 상태 조회가 필요합니다. 연결을 확인하고 새로고침하세요."} ${error.message}`
      : error.message);
    byId("error-banner").hidden = false;
  }
  async function refresh(internal = false) {
    if (state.refreshing || (state.busy && !internal)) return;
    state.refreshing = true;
    lockControls();
    try {
      // Fetch capabilities first: the snapshot/ETag is the last observation.
      // RF capability discovery can observe an external LO change. Fetch target
      // limits afterwards so they use that newly verified carrier context.
      const capabilities = [];
      for (const url of ["/api/v1/rf/capabilities", "/api/v1/rte/capabilities"]) {
        try { capabilities.push({status: "fulfilled", value: await request(url)}); }
        catch (reason) { capabilities.push({status: "rejected", reason}); }
      }
      const failure = capabilities.find(result => result.status === "rejected");
      if (!failure) acceptCapabilities(capabilities[0].value.data, capabilities[1].value.data);
      // A capability failure must not hide a fresh degraded/unknown snapshot.
      acceptSnapshot(await request("/api/v1/system"));
      if (failure) throw failure.reason;
      connection(true);
      text("last-sync", new Date().toLocaleTimeString("ko-KR"));
    } catch (error) {
      connection(false);
      showError(error);
    } finally {
      state.refreshing = false;
      lockControls();
    }
  }
  function numericPayload(fields) {
    const payload = {};
    Object.entries(fields).forEach(([key, id]) => {
      const raw = byId(id).value.trim();
      const number = Number(raw);
      if (!raw || !Number.isFinite(number)) throw new Error("모든 입력에 유효한 숫자를 입력하세요.");
      payload[key] = number;
    });
    return payload;
  }
  async function mutate(kind, id) {
    if (state.busy || state.refreshing || !state.synced || !state.etag || state.snapshot?.degraded) return;
    if (kind !== "rf" && (kind !== "target" || !targetIds.includes(id))) return;
    const form = kind === "target" ? targetElement(id, "form") : byId("rf-form");
    const status = kind === "target" ? targetElement(id, "status") : byId("rf-status");
    if (!form.reportValidity()) return;
    let payload;
    try {
      if (kind === "target") {
        captureTarget(id);
        payload = {enabled: targetElement(id, "enabled").checked, ...numericPayload(targetInputs(id))};
      } else {
        payload = numericPayload(rfFields);
        payload.carrier_hz = Math.round(payload.carrier_hz * 1e6);
        payload.bandwidth_hz = Math.round(payload.bandwidth_hz * 1e6);
      }
    } catch (error) { showError(error); return; }
    state.busy = true;
    byId("error-banner").hidden = true;
    status.textContent = "적용 중…";
    lockControls();
    try {
      const response = await request(kind === "target" ? `/api/v1/rte/targets/${id}/load-param` : "/api/v1/rf/config",
        kind === "target" ? "POST" : "PATCH", payload);
      if (kind === "target") state.targetDrafts[id].dirty = false;
      else state.rfDirty = false;
      acceptSnapshot(response);
      status.textContent = kind === "target" ? `표적 ${id} 적용 완료` : "RF 적용 완료";
      await refresh(true);
    } catch (error) {
      connection(false);
      // Completion can be ambiguous on timeout; always resync, never retry LP.
      await refresh(true);
      showError(error);
      status.textContent = "편집값과 최신 적용 상태를 확인하세요.";
    } finally {
      state.busy = false;
      lockControls();
    }
  }
  function start() {
    byId("dismiss-error").addEventListener("click", () => { byId("error-banner").hidden = true; });
    targetIds.forEach(id => {
      [...Object.values(targetFields), "enabled"].forEach(name =>
        targetElement(id, name).addEventListener("input", () => captureTarget(id)));
      targetElement(id, "form").addEventListener("submit", event => {
        event.preventDefault(); return mutate("target", id);
      });
      targetElement(id, "revert").addEventListener("click", () => {
        if (state.busy || state.refreshing || !state.snapshot) return;
        state.targetDrafts[id] = draftFromTarget(target(id));
        targetElement(id, "status").textContent = "";
        renderTargetForm(id);
      });
    });
    Object.values(rfFields).forEach(id => byId(id).addEventListener("input", () => {
      state.rfDirty = true;
      text("rf-status", "미적용 RF 편집값");
    }));
    byId("rf-form").addEventListener("submit", event => { event.preventDefault(); void mutate("rf"); });
    byId("rf-revert").addEventListener("click", () => { if (!state.busy && !state.refreshing) { state.rfDirty = false; renderRfForm(); text("rf-status", ""); } });
    byId("refresh-button").addEventListener("click", () => { void refresh(); });
    state.interval = clock.setInterval(() => { void refresh(); }, POLL_MS);
    return refresh();
  }
  return {start, refresh, mutate, stop: () => clock.clearInterval(state.interval)};
}

if (typeof module !== "undefined" && module.exports) module.exports = {createRteApp};
else void createRteApp(document, window.fetch.bind(window)).start();

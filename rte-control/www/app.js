"use strict";

const API = Object.freeze({
  health: "/healthz",
  system: "/api/v1/system",
  capabilities: "/api/v1/rte/capabilities",
  config: "/api/v1/config",
});

const REQUEST_TIMEOUT_MS = 8000;
const REFRESH_INTERVAL_MS = 5000;

const INPUT_IDS = Object.freeze([
  "rf-sample-rate",
  "rf-carrier",
  "range-m",
  "velocity-mps",
  "loss-db",
  "resp-amplitude",
  "resp-frequency",
  "resp-phase",
  "heart-amplitude",
  "heart-frequency",
  "heart-phase",
]);

const state = {
  snapshot: null,
  capabilities: null,
  health: null,
  etag: null,
  editEtag: null,
  online: false,
  dirty: false,
  busy: false,
  refreshing: false,
  formInitialized: false,
  capabilitiesError: "",
  healthError: "",
  pollTimer: null,
};

const dom = {};

class ApiError extends Error {
  constructor(message, options = {}) {
    super(message);
    this.name = "ApiError";
    this.status = options.status || 0;
    this.code = options.code || "";
    this.field = options.field || "";
    this.revision = options.revision;
    this.etag = options.etag || null;
  }
}

document.addEventListener("DOMContentLoaded", initialize);

function initialize() {
  collectDom();
  bindEvents();
  updateInputPreviews();
  renderAll();
  void refreshDashboard({ preserveForm: false, quiet: false });
  schedulePoll();
}

function collectDom() {
  const ids = [
    "connection-pill", "connection-text", "health-pill", "health-text",
    "revision-pill", "last-sync", "error-banner", "error-title",
    "error-message", "error-meta", "dismiss-error", "warning-banner",
    "warning-message", "degraded-banner", "initialization-banner", "config-form",
    "apply-button", "apply-button-text", "revert-button", "refresh-button",
    "form-status", "sample-rate-preview", "carrier-preview",
    "actual-sample-rate", "actual-carrier", "actual-rx-lo", "actual-tx-lo",
    "hardware-build", "hardware-state", "capability-revision", "max-range",
    "max-velocity", "phase-resolution", "frequency-resolution",
    "max-motion-amplitude", "capability-state", "requested-empty",
    "requested-table-wrap", "requested-table-body", "applied-empty",
    "applied-table-wrap", "applied-table-body", "register-empty",
    "register-table-wrap", "register-table-body", "register-count",
  ];

  for (const id of [...ids, ...INPUT_IDS]) {
    const element = document.getElementById(id);
    if (!element) {
      throw new Error(`Required UI element is missing: ${id}`);
    }
    dom[id] = element;
  }
}

function bindEvents() {
  dom["config-form"].addEventListener("submit", handleSubmit);
  dom["config-form"].addEventListener("input", handleFormInput);
  dom["revert-button"].addEventListener("click", revertForm);
  dom["refresh-button"].addEventListener("click", () => {
    void refreshDashboard({ preserveForm: state.dirty, quiet: false });
  });
  dom["dismiss-error"].addEventListener("click", hideError);
  document.addEventListener("visibilitychange", handleVisibilityChange);
  window.addEventListener("beforeunload", handleBeforeUnload);
}

function handleVisibilityChange() {
  if (document.visibilityState === "visible" && !state.busy) {
    void refreshDashboard({ preserveForm: state.dirty, quiet: true });
  }
  schedulePoll();
}

function handleBeforeUnload(event) {
  if (!state.dirty) {
    return;
  }
  event.preventDefault();
  event.returnValue = "";
}

function schedulePoll() {
  if (state.pollTimer !== null) {
    window.clearTimeout(state.pollTimer);
  }
  state.pollTimer = window.setTimeout(async () => {
    if (document.visibilityState === "visible" && !state.busy) {
      await refreshDashboard({ preserveForm: state.dirty, quiet: true });
    }
    schedulePoll();
  }, REFRESH_INTERVAL_MS);
}

async function requestJson(path, options = {}) {
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  const headers = new Headers(options.headers || {});
  const acceptedStatuses = new Set(options.acceptStatuses || []);

  if (!headers.has("Accept")) {
    headers.set("Accept", "application/json");
  }

  try {
    const response = await fetch(path, {
      method: options.method || "GET",
      credentials: "same-origin",
      cache: "no-store",
      headers,
      body: options.body,
      signal: controller.signal,
    });
    const rawBody = await response.text();
    let data = null;

    if (rawBody.trim() !== "") {
      try {
        data = JSON.parse(rawBody);
      } catch (error) {
        throw new ApiError("서버가 유효하지 않은 JSON을 반환했습니다.", {
          status: response.status,
          etag: response.headers.get("ETag"),
        });
      }
    }

    if (!response.ok && !acceptedStatuses.has(response.status)) {
      const detail = data && data.error && typeof data.error === "object" ? data.error : {};
      throw new ApiError(
        typeof detail.message === "string" ? detail.message : `HTTP ${response.status} 요청 실패`,
        {
          status: response.status,
          code: typeof detail.code === "string" ? detail.code : "",
          field: typeof detail.field === "string" ? detail.field : "",
          revision: data && Number.isSafeInteger(data.revision) ? data.revision : undefined,
          etag: response.headers.get("ETag"),
        },
      );
    }

    if (data === null || typeof data !== "object" || Array.isArray(data)) {
      throw new ApiError("서버 응답에 JSON 객체가 없습니다.", {
        status: response.status,
        etag: response.headers.get("ETag"),
      });
    }

    return {
      data,
      status: response.status,
      etag: response.headers.get("ETag"),
    };
  } catch (error) {
    if (error instanceof ApiError) {
      throw error;
    }
    if (error && error.name === "AbortError") {
      throw new ApiError("서버 응답 시간이 초과되었습니다.");
    }
    throw new ApiError("RTE 제어 서비스에 연결할 수 없습니다.");
  } finally {
    window.clearTimeout(timeout);
  }
}

async function refreshDashboard(options = {}) {
  const preserveForm = options.preserveForm === true;
  const quiet = options.quiet === true;

  if (state.refreshing || state.busy) {
    return;
  }

  state.refreshing = true;
  if (!state.online) {
    setConnection("loading", "연결 중");
  }
  updateControls();
  let systemSucceeded = false;

  try {
    const [systemResult, capabilityResult, healthResult] = await Promise.allSettled([
      requestJson(API.system),
      requestJson(API.capabilities),
      requestJson(API.health, { acceptStatuses: [503] }),
    ]);

    if (systemResult.status === "fulfilled") {
      try {
        applySnapshotResponse(systemResult.value, {
          populateForm: !preserveForm,
          adoptEditRevision: !preserveForm || state.editEtag === null,
        });
        systemSucceeded = true;
        state.online = true;
        setLastSync(new Date());
        if (!quiet) {
          hideError();
          setFormStatus(preserveForm && state.dirty
            ? editRevisionMessage("서버 상태를 갱신했고 편집값은 유지했습니다.")
            : "최신 상태를 읽었습니다.");
        }
      } catch (error) {
        state.online = false;
        setConnection("danger", "응답 오류");
        if (!quiet || state.snapshot === null) {
          showApiError("시스템 응답 형식 오류", error, !quiet);
        }
      }
    } else {
      state.online = false;
      setConnection("danger", "연결 끊김");
      if (!quiet || state.snapshot === null) {
        showApiError("제어 서비스 연결 실패", systemResult.reason, !quiet);
      }
    }

    if (capabilityResult.status === "fulfilled") {
      try {
        validateCapabilitiesShape(capabilityResult.value.data);
        state.capabilities = capabilityResult.value.data;
        state.capabilitiesError = "";
      } catch (error) {
        state.capabilities = null;
        state.capabilitiesError = messageFromError(error);
      }
    } else {
      state.capabilitiesError = messageFromError(capabilityResult.reason);
    }

    if (healthResult.status === "fulfilled") {
      try {
        validateHealthShape(healthResult.value.data);
        state.health = healthResult.value.data;
        state.healthError = "";
      } catch (error) {
        state.health = null;
        state.healthError = messageFromError(error);
      }
    } else {
      state.health = null;
      state.healthError = messageFromError(healthResult.reason);
    }
  } catch (error) {
    state.online = false;
    if (!quiet || state.snapshot === null) {
      showApiError("상태 갱신 실패", error, !quiet);
    }
  } finally {
    state.refreshing = false;
    renderAll();
    updateControls();
  }

  if (systemSucceeded && state.dirty && state.editEtag !== state.etag && quiet) {
    setFormStatus(editRevisionMessage("다른 클라이언트의 새 revision을 감지했습니다."));
  }
}

function applySnapshotResponse(response, options = {}) {
  const snapshot = response.data;
  validateSnapshotShape(snapshot);

  const nextEtag = validEtag(response.etag, snapshot.revision);
  if (options.populateForm === true) {
    populateForm(snapshot);
  }

  state.snapshot = snapshot;
  state.etag = nextEtag;
  if (options.adoptEditRevision === true) {
    state.editEtag = state.etag;
  }

  if (options.populateForm === true) {
    state.formInitialized = true;
    state.dirty = false;
    state.editEtag = state.etag;
  }
}

function validateSnapshotShape(snapshot) {
  if (!snapshot || typeof snapshot !== "object" ||
      !Number.isSafeInteger(snapshot.revision) ||
      typeof snapshot.hardware_state_known !== "boolean" ||
      !snapshot.rf || typeof snapshot.rf !== "object" ||
      !("requested" in snapshot) || !("applied" in snapshot)) {
    throw new ApiError("시스템 snapshot 형식이 API 계약과 일치하지 않습니다.");
  }
  if (!numbersAreFinite(snapshot.rf, [
    "sample_rate_hz", "carrier_hz", "rx_lo_hz", "tx_lo_hz",
  ])) {
    throw new ApiError("시스템 RF snapshot에 유효하지 않은 숫자가 있습니다.");
  }
  if (snapshot.requested === null || snapshot.applied === null) {
    if (snapshot.requested !== null || snapshot.applied !== null) {
      throw new ApiError("requested와 applied의 초기화 상태가 서로 다릅니다.");
    }
    return;
  }
  if (!motionConfigIsValid(snapshot.requested.respiration) ||
      !motionConfigIsValid(snapshot.requested.heartbeat) ||
      !numbersAreFinite(snapshot.requested, [
        "range_m", "radial_velocity_mps", "loss_db",
      ]) || !numbersAreFinite(snapshot.applied.range, [
        "delay_samples", "delay_range_m", "phase_rad",
      ]) || !numbersAreFinite(snapshot.applied, [
        "doppler_hz", "radial_velocity_mps", "scaling_raw", "linear_gain",
      ]) || typeof snapshot.applied.muted !== "boolean" ||
      (!snapshot.applied.muted && !Number.isFinite(snapshot.applied.loss_db)) ||
      !motionAppliedIsValid(snapshot.applied.respiration) ||
      !motionAppliedIsValid(snapshot.applied.heartbeat) ||
      !snapshot.applied.registers || typeof snapshot.applied.registers !== "object" ||
      Array.isArray(snapshot.applied.registers)) {
    throw new ApiError("RTE snapshot 형식이 API 계약과 일치하지 않습니다.");
  }
}

function numbersAreFinite(object, keys) {
  return Boolean(object && typeof object === "object" &&
    keys.every((key) => Number.isFinite(object[key])));
}

function motionConfigIsValid(motion) {
  return numbersAreFinite(motion, ["amplitude_m", "frequency_hz", "phase_rad"]);
}

function motionAppliedIsValid(motion) {
  return numbersAreFinite(motion, [
    "amplitude_m", "frequency_hz", "phase_rad", "phase_gain_cycles", "max_doppler_hz",
  ]);
}

function validateCapabilitiesShape(capabilities) {
  if (!capabilities || !Number.isSafeInteger(capabilities.revision) ||
      !numbersAreFinite(capabilities, [
        "max_range_m", "max_abs_velocity_mps", "phase_resolution_rad",
        "frequency_resolution_hz", "max_motion_amplitude_m",
      ])) {
    throw new ApiError("Capability 응답 형식이 API 계약과 일치하지 않습니다.");
  }
}

function validateHealthShape(health) {
  if (!health || !["ok", "degraded"].includes(health.status) ||
      typeof health.hardware_state_known !== "boolean") {
    throw new ApiError("Health 응답 형식이 API 계약과 일치하지 않습니다.");
  }
}

function validEtag(headerValue, revision) {
  if (typeof headerValue === "string" && /^"[0-9]+"$/.test(headerValue)) {
    return headerValue;
  }
  return `"${revision}"`;
}

async function refreshAuxiliaryState() {
  const [capabilityResult, healthResult] = await Promise.allSettled([
    requestJson(API.capabilities),
    requestJson(API.health, { acceptStatuses: [503] }),
  ]);

  if (capabilityResult.status === "fulfilled") {
    try {
      validateCapabilitiesShape(capabilityResult.value.data);
      state.capabilities = capabilityResult.value.data;
      state.capabilitiesError = "";
    } catch (error) {
      state.capabilities = null;
      state.capabilitiesError = messageFromError(error);
    }
  } else {
    state.capabilitiesError = messageFromError(capabilityResult.reason);
  }

  if (healthResult.status === "fulfilled") {
    try {
      validateHealthShape(healthResult.value.data);
      state.health = healthResult.value.data;
      state.healthError = "";
    } catch (error) {
      state.health = null;
      state.healthError = messageFromError(error);
    }
  } else {
    state.health = null;
    state.healthError = messageFromError(healthResult.reason);
  }
}

async function handleSubmit(event) {
  event.preventDefault();
  validateFormAgainstCurrentCapabilities();

  if (!dom["config-form"].checkValidity()) {
    dom["config-form"].reportValidity();
    setFormStatus("표시된 입력값을 확인하세요.");
    return;
  }
  if (!state.online || state.editEtag === null) {
    showError("적용할 수 없습니다", "먼저 RTE 제어 서비스의 최신 상태를 읽어야 합니다.", "", true);
    return;
  }
  if (writesAreBlocked()) {
    showError(
      "하드웨어 상태를 확인할 수 없습니다",
      "복구에 실패한 상태에서는 추가 쓰기가 차단됩니다. 서비스를 재시작하고 하드웨어를 확인하세요.",
      "hardware_state_known=false",
      true,
    );
    return;
  }

  let payload;
  try {
    payload = buildCombinedPatch();
  } catch (error) {
    showError("입력값 오류", messageFromError(error), "", true);
    return;
  }

  state.busy = true;
  setConnection("loading", "적용 중");
  setFormStatus(`REV ${etagNumber(state.editEtag)} 기준 트랜잭션을 적용하는 중…`);
  updateControls();

  try {
    const response = await requestJson(API.config, {
      method: "PATCH",
      headers: {
        "Content-Type": "application/json",
        "If-Match": state.editEtag,
      },
      body: JSON.stringify(payload),
    });

    applySnapshotResponse(response, {
      populateForm: true,
      adoptEditRevision: true,
    });
    state.online = true;
    await refreshAuxiliaryState();
    setLastSync(new Date());
    hideError();
    setFormStatus(`REV ${state.snapshot.revision}에 RF와 RTE를 함께 적용했습니다.`);
  } catch (error) {
    if (error instanceof ApiError && error.status === 409) {
      const synchronized = await synchronizePreservingForm(true);
      const suffix = synchronized
        ? " 최신 revision을 읽었으며 편집값은 유지했습니다. 값을 검토한 뒤 다시 적용하세요."
        : " 최신 상태를 다시 읽지 못했습니다.";
      showApiError("Revision 충돌", new ApiError(`${error.message}${suffix}`, {
        status: error.status,
        code: error.code,
        field: error.field,
        revision: error.revision,
      }), true);
      setFormStatus(editRevisionMessage("동시 수정 충돌이 발생했습니다."));
    } else {
      const resultIsUncertain = !(error instanceof ApiError &&
        error.status >= 400 && error.status < 500);
      let synchronized = false;
      if (resultIsUncertain) {
        synchronized = await synchronizePreservingForm(false);
      }
      showApiError("구성 적용 실패", error, true);
      setFormStatus(resultIsUncertain
        ? synchronized
          ? "적용 결과를 단정할 수 없어 하드웨어 snapshot을 다시 읽었습니다."
          : "적용 결과를 단정할 수 없으며 하드웨어 snapshot 재조회도 실패했습니다."
        : "요청이 완료되지 않았습니다. 오류 내용을 확인하세요.");
    }
  } finally {
    state.busy = false;
    renderAll();
    updateControls();
  }
}

async function synchronizePreservingForm(adoptEditRevision) {
  try {
    const response = await requestJson(API.system);
    applySnapshotResponse(response, {
      populateForm: false,
      adoptEditRevision,
    });
    state.online = true;
    setLastSync(new Date());
    await refreshAuxiliaryState();
    return true;
  } catch (error) {
    state.online = false;
    return false;
  }
}

function buildCombinedPatch() {
  return {
    rf: {
      sample_rate_hz: readInputNumber("rf-sample-rate", "샘플 레이트", { integer: true, positive: true }),
      carrier_hz: readInputNumber("rf-carrier", "반송파", { integer: true, positive: true }),
    },
    rte: {
      range_m: readInputNumber("range-m", "거리", { nonnegative: true }),
      radial_velocity_mps: readInputNumber("velocity-mps", "반경 속도"),
      loss_db: readInputNumber("loss-db", "손실", { nonnegative: true }),
      respiration: {
        amplitude_m: readInputNumber("resp-amplitude", "호흡 진폭", { nonnegative: true }),
        frequency_hz: readInputNumber("resp-frequency", "호흡 주파수", { nonnegative: true }),
        phase_rad: readInputNumber("resp-phase", "호흡 위상"),
      },
      heartbeat: {
        amplitude_m: readInputNumber("heart-amplitude", "심박 진폭", { nonnegative: true }),
        frequency_hz: readInputNumber("heart-frequency", "심박 주파수", { nonnegative: true }),
        phase_rad: readInputNumber("heart-phase", "심박 위상"),
      },
    },
  };
}

function readInputNumber(id, label, options = {}) {
  const input = dom[id];
  const raw = input.value.trim();
  const value = Number(raw);

  if (raw === "" || !Number.isFinite(value)) {
    throw new Error(`${label}에 유한한 숫자를 입력하세요.`);
  }
  if (options.integer === true && !Number.isSafeInteger(value)) {
    throw new Error(`${label}는 2^53−1 이하의 정수여야 합니다.`);
  }
  if (options.positive === true && value <= 0) {
    throw new Error(`${label}는 0보다 커야 합니다.`);
  }
  if (options.nonnegative === true && value < 0) {
    throw new Error(`${label}는 0 이상이어야 합니다.`);
  }
  return value;
}

function handleFormInput(event) {
  if (!(event.target instanceof HTMLInputElement)) {
    return;
  }
  event.target.setCustomValidity("");
  state.dirty = true;
  updateInputPreviews();
  validateFormAgainstCurrentCapabilities();
  setFormStatus(editRevisionMessage("적용되지 않은 편집 내용이 있습니다."));
  updateControls();
}

function validateFormAgainstCurrentCapabilities() {
  const constrainedIds = [
    "rf-sample-rate", "rf-carrier", "range-m", "velocity-mps",
    "resp-amplitude", "heart-amplitude",
  ];
  for (const id of constrainedIds) {
    dom[id].setCustomValidity("");
  }

  validateSafeInteger(dom["rf-sample-rate"], "샘플 레이트");
  validateSafeInteger(dom["rf-carrier"], "반송파");

  if (!state.snapshot || !state.capabilities ||
      state.capabilities.revision !== state.snapshot.revision ||
      !formUsesCurrentRf()) {
    return;
  }

  applyMaximum("range-m", state.capabilities.max_range_m, "현재 RF에서 지원되는 최대 거리를 초과했습니다.");
  applyAbsoluteMaximum(
    "velocity-mps",
    state.capabilities.max_abs_velocity_mps,
    "현재 RF에서 지원되는 최대 절대 속도를 초과했습니다.",
  );
  applyMaximum(
    "resp-amplitude",
    state.capabilities.max_motion_amplitude_m,
    "현재 반송파에서 지원되는 최대 motion 진폭을 초과했습니다.",
  );
  applyMaximum(
    "heart-amplitude",
    state.capabilities.max_motion_amplitude_m,
    "현재 반송파에서 지원되는 최대 motion 진폭을 초과했습니다.",
  );
}

function validateSafeInteger(input, label) {
  if (input.value.trim() === "") {
    return;
  }
  const value = Number(input.value);
  if (Number.isFinite(value) && !Number.isSafeInteger(value)) {
    input.setCustomValidity(`${label}는 2^53−1 이하의 정수여야 합니다.`);
  }
}

function formUsesCurrentRf() {
  const rf = state.snapshot.rf;
  return Number(dom["rf-sample-rate"].value) === rf.sample_rate_hz &&
    Number(dom["rf-carrier"].value) === rf.carrier_hz;
}

function applyMaximum(id, maximum, message) {
  const value = Number(dom[id].value);
  if (Number.isFinite(value) && Number.isFinite(maximum) && value > maximum) {
    dom[id].setCustomValidity(message);
  }
}

function applyAbsoluteMaximum(id, maximum, message) {
  const value = Number(dom[id].value);
  if (Number.isFinite(value) && Number.isFinite(maximum) && Math.abs(value) > maximum) {
    dom[id].setCustomValidity(message);
  }
}

function populateForm(snapshot) {
  setInputValue("rf-sample-rate", snapshot.rf.sample_rate_hz);
  setInputValue("rf-carrier", snapshot.rf.carrier_hz);

  if (snapshot.requested === null) {
    for (const id of INPUT_IDS.slice(2)) {
      dom[id].value = "";
    }
  } else {
    const requested = snapshot.requested;
    setInputValue("range-m", requested.range_m);
    setInputValue("velocity-mps", requested.radial_velocity_mps);
    setInputValue("loss-db", requested.loss_db);
    setInputValue("resp-amplitude", requested.respiration.amplitude_m);
    setInputValue("resp-frequency", requested.respiration.frequency_hz);
    setInputValue("resp-phase", requested.respiration.phase_rad);
    setInputValue("heart-amplitude", requested.heartbeat.amplitude_m);
    setInputValue("heart-frequency", requested.heartbeat.frequency_hz);
    setInputValue("heart-phase", requested.heartbeat.phase_rad);
  }

  for (const id of INPUT_IDS) {
    dom[id].setCustomValidity("");
  }
  updateInputPreviews();
  validateFormAgainstCurrentCapabilities();
}

function setInputValue(id, value) {
  dom[id].value = Number.isFinite(value) ? String(value) : "";
}

function revertForm() {
  if (!state.snapshot) {
    return;
  }
  populateForm(state.snapshot);
  state.dirty = false;
  state.editEtag = state.etag;
  hideError();
  setFormStatus(state.snapshot.requested === null
    ? "초기 RTE 필드를 모두 입력하세요."
    : `REV ${state.snapshot.revision} 요청값으로 되돌렸습니다.`);
  updateControls();
}

function updateInputPreviews() {
  dom["sample-rate-preview"].textContent = previewFrequency(dom["rf-sample-rate"].value);
  dom["carrier-preview"].textContent = previewFrequency(dom["rf-carrier"].value);
}

function previewFrequency(raw) {
  if (raw.trim() === "") {
    return "—";
  }
  const value = Number(raw);
  return Number.isFinite(value) ? formatFrequency(value) : "유효한 숫자가 아닙니다";
}

function renderAll() {
  renderConnection();
  renderHealth();
  renderSnapshot();
  renderDegradedState();
  renderCapabilities();
  validateFormAgainstCurrentCapabilities();
  updateControls();
}

function renderConnection() {
  if (state.refreshing && !state.online) {
    setConnection("loading", "연결 중");
  } else if (state.online) {
    setConnection(state.busy ? "loading" : "ok", state.busy ? "적용 중" : "API 연결됨");
  } else {
    setConnection("danger", "연결 끊김");
  }
}

function setConnection(tone, text) {
  dom["connection-pill"].dataset.tone = tone;
  dom["connection-text"].textContent = text;
}

function renderHealth() {
  let tone = "idle";
  let text = "상태 확인 중";

  if (state.health && state.health.status === "degraded") {
    tone = "danger";
    text = "하드웨어 degraded";
  } else if (state.snapshot && state.snapshot.requested === null) {
    tone = "warning";
    text = "RTE 초기화 필요";
  } else if (state.health && state.health.status === "ok" && state.health.hardware_state_known) {
    tone = "ok";
    text = "하드웨어 정상";
  } else if (state.healthError) {
    tone = "warning";
    text = "Health 확인 실패";
  } else if (state.snapshot && state.snapshot.hardware_state_known === false) {
    tone = "danger";
    text = "하드웨어 상태 불명";
  }

  dom["health-pill"].dataset.tone = tone;
  dom["health-text"].textContent = text;
}

function renderDegradedState() {
  dom["degraded-banner"].hidden = !writesAreBlocked();
}

function renderSnapshot() {
  const snapshot = state.snapshot;

  if (!snapshot) {
    dom["revision-pill"].textContent = "REV —";
    dom["initialization-banner"].hidden = true;
    clearSystemMetrics();
    renderRequested(null);
    renderApplied(null);
    renderRegisters(null);
    renderWarning(null);
    return;
  }

  dom["revision-pill"].textContent = `REV ${snapshot.revision}`;
  dom["initialization-banner"].hidden = snapshot.requested !== null;
  dom["actual-sample-rate"].textContent = formatFrequency(snapshot.rf.sample_rate_hz);
  dom["actual-carrier"].textContent = formatFrequency(snapshot.rf.carrier_hz);
  dom["actual-rx-lo"].textContent = formatFrequency(snapshot.rf.rx_lo_hz);
  dom["actual-tx-lo"].textContent = formatFrequency(snapshot.rf.tx_lo_hz);
  dom["hardware-build"].textContent = formatBuildId(snapshot.hardware_build_id);
  dom["hardware-state"].textContent = hardwareStateLabel(snapshot);
  dom["apply-button-text"].textContent = snapshot.requested === null ? "첫 구성 적용" : "구성 적용";

  renderRequested(snapshot.requested);
  renderApplied(snapshot.applied);
  renderRegisters(snapshot.applied ? snapshot.applied.registers : null);
  renderWarning(snapshot.warning);
}

function clearSystemMetrics() {
  for (const id of [
    "actual-sample-rate", "actual-carrier", "actual-rx-lo", "actual-tx-lo",
    "hardware-build", "hardware-state",
  ]) {
    dom[id].textContent = "—";
  }
}

function hardwareStateLabel(snapshot) {
  if (snapshot.requested === null) {
    return "초기 구성 대기";
  }
  return snapshot.hardware_state_known ? "Known / writable" : "Unknown / blocked";
}

function renderCapabilities() {
  const capabilities = state.capabilities;

  if (!capabilities) {
    dom["capability-revision"].textContent = "REV —";
    for (const id of [
      "max-range", "max-velocity", "phase-resolution",
      "frequency-resolution", "max-motion-amplitude",
    ]) {
      dom[id].textContent = "—";
    }
  } else {
    dom["capability-revision"].textContent = `REV ${capabilities.revision}`;
    dom["max-range"].textContent = formatWithUnit(capabilities.max_range_m, "m");
    dom["max-velocity"].textContent = formatWithUnit(capabilities.max_abs_velocity_mps, "m/s");
    dom["phase-resolution"].textContent = formatWithUnit(capabilities.phase_resolution_rad, "rad");
    dom["frequency-resolution"].textContent = formatWithUnit(capabilities.frequency_resolution_hz, "Hz");
    dom["max-motion-amplitude"].textContent = formatWithUnit(capabilities.max_motion_amplitude_m, "m");
  }

  dom["capability-state"].textContent = state.capabilitiesError
    ? `${capabilities ? "마지막 성공값 표시 · " : ""}갱신 실패: ${state.capabilitiesError}`
    : capabilities && state.snapshot && capabilities.revision !== state.snapshot.revision
      ? "시스템 snapshot과 capability revision이 다릅니다. 다음 동기화를 기다리세요."
      : "";
}

function renderRequested(requested) {
  dom["requested-empty"].hidden = requested !== null;
  dom["requested-table-wrap"].hidden = requested === null;
  dom["requested-table-body"].replaceChildren();

  if (requested === null) {
    return;
  }

  appendRows(dom["requested-table-body"], [
    ["거리", formatWithUnit(requested.range_m, "m")],
    ["반경 속도", formatWithUnit(requested.radial_velocity_mps, "m/s")],
    ["손실", formatWithUnit(requested.loss_db, "dB")],
    ["호흡 · 진폭", formatWithUnit(requested.respiration.amplitude_m, "m")],
    ["호흡 · 주파수", formatWithUnit(requested.respiration.frequency_hz, "Hz")],
    ["호흡 · 위상", formatWithUnit(requested.respiration.phase_rad, "rad")],
    ["심박 · 진폭", formatWithUnit(requested.heartbeat.amplitude_m, "m")],
    ["심박 · 주파수", formatWithUnit(requested.heartbeat.frequency_hz, "Hz")],
    ["심박 · 위상", formatWithUnit(requested.heartbeat.phase_rad, "rad")],
  ]);
}

function renderApplied(applied) {
  dom["applied-empty"].hidden = applied !== null;
  dom["applied-table-wrap"].hidden = applied === null;
  dom["applied-table-body"].replaceChildren();

  if (applied === null) {
    return;
  }

  const loss = applied.muted
    ? "muted · loss undefined"
    : formatWithUnit(applied.loss_db, "dB");
  appendRows(dom["applied-table-body"], [
    ["거리 · delay", `${formatInteger(applied.range.delay_samples)} samples`],
    ["거리 · 양자화", formatWithUnit(applied.range.delay_range_m, "m")],
    ["거리 · 위상", formatWithUnit(applied.range.phase_rad, "rad")],
    ["Doppler", formatWithUnit(applied.doppler_hz, "Hz")],
    ["반경 속도", formatWithUnit(applied.radial_velocity_mps, "m/s")],
    ["Gain · scaling raw", formatInteger(applied.scaling_raw)],
    ["Gain · linear", formatNumber(applied.linear_gain)],
    ["Gain · loss", loss],
    ["호흡 · 진폭", formatWithUnit(applied.respiration.amplitude_m, "m")],
    ["호흡 · 주파수", formatWithUnit(applied.respiration.frequency_hz, "Hz")],
    ["호흡 · 위상", formatWithUnit(applied.respiration.phase_rad, "rad")],
    ["호흡 · phase gain", formatWithUnit(applied.respiration.phase_gain_cycles, "cycles")],
    ["호흡 · max Doppler", formatWithUnit(applied.respiration.max_doppler_hz, "Hz")],
    ["심박 · 진폭", formatWithUnit(applied.heartbeat.amplitude_m, "m")],
    ["심박 · 주파수", formatWithUnit(applied.heartbeat.frequency_hz, "Hz")],
    ["심박 · 위상", formatWithUnit(applied.heartbeat.phase_rad, "rad")],
    ["심박 · phase gain", formatWithUnit(applied.heartbeat.phase_gain_cycles, "cycles")],
    ["심박 · max Doppler", formatWithUnit(applied.heartbeat.max_doppler_hz, "Hz")],
  ]);
}

function appendRows(tableBody, rows) {
  const fragment = document.createDocumentFragment();
  for (const [label, value] of rows) {
    const row = document.createElement("tr");
    const labelCell = document.createElement("th");
    const valueCell = document.createElement("td");
    labelCell.scope = "row";
    labelCell.textContent = label;
    valueCell.textContent = value;
    row.append(labelCell, valueCell);
    fragment.append(row);
  }
  tableBody.append(fragment);
}

function renderRegisters(registers) {
  const entries = registers && typeof registers === "object" ? Object.entries(registers) : [];
  dom["register-count"].textContent = String(entries.length);
  dom["register-empty"].hidden = entries.length !== 0;
  dom["register-table-wrap"].hidden = entries.length === 0;
  dom["register-table-body"].replaceChildren();

  const fragment = document.createDocumentFragment();
  for (const [name, word] of entries) {
    const row = document.createElement("tr");
    const nameCell = document.createElement("th");
    const wordCell = document.createElement("td");
    const code = document.createElement("code");
    nameCell.scope = "row";
    nameCell.textContent = name;
    code.textContent = String(word);
    wordCell.append(code);
    row.append(nameCell, wordCell);
    fragment.append(row);
  }
  dom["register-table-body"].append(fragment);
}

function renderWarning(warning) {
  const visible = typeof warning === "string" && warning !== "";
  dom["warning-banner"].hidden = !visible;
  dom["warning-message"].textContent = visible ? warning : "";
}

function updateControls() {
  const blocked = writesAreBlocked();
  dom["apply-button"].disabled = state.busy || state.refreshing || !state.online ||
    state.editEtag === null || blocked;
  dom["refresh-button"].disabled = state.busy || state.refreshing;
  dom["revert-button"].disabled = state.busy || !state.dirty || state.snapshot === null;

  if (state.busy) {
    dom["apply-button-text"].textContent = "적용 중…";
  } else if (state.snapshot) {
    dom["apply-button-text"].textContent = state.snapshot.requested === null ? "첫 구성 적용" : "구성 적용";
  }
}

function writesAreBlocked() {
  return Boolean((state.health && state.health.status === "degraded") ||
    (state.snapshot && state.snapshot.requested !== null &&
      state.snapshot.hardware_state_known === false));
}

function editRevisionMessage(prefix) {
  const editRevision = etagNumber(state.editEtag);
  const serverRevision = etagNumber(state.etag);
  if (editRevision !== null && serverRevision !== null && editRevision !== serverRevision) {
    return `${prefix} 편집 기준 REV ${editRevision}, 현재 서버 REV ${serverRevision}.`;
  }
  return prefix;
}

function etagNumber(etag) {
  if (typeof etag !== "string") {
    return null;
  }
  const match = /^"([0-9]+)"$/.exec(etag);
  return match ? match[1] : null;
}

function setFormStatus(message) {
  dom["form-status"].textContent = message;
}

function setLastSync(date) {
  dom["last-sync"].dateTime = date.toISOString();
  dom["last-sync"].textContent = new Intl.DateTimeFormat("ko-KR", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  }).format(date);
}

function showApiError(title, error, focus) {
  if (error instanceof ApiError) {
    const metadata = [];
    if (error.status) {
      metadata.push(`HTTP ${error.status}`);
    }
    if (error.code) {
      metadata.push(error.code);
    }
    if (error.field) {
      metadata.push(`field: ${error.field}`);
    }
    if (error.revision !== undefined) {
      metadata.push(`server revision: ${error.revision}`);
    }
    showError(title, error.message, metadata.join(" · "), focus);
  } else {
    showError(title, messageFromError(error), "", focus);
  }
}

function showError(title, message, metadata, focus) {
  dom["error-title"].textContent = title;
  dom["error-message"].textContent = message;
  dom["error-meta"].textContent = metadata;
  dom["error-meta"].hidden = metadata === "";
  dom["error-banner"].hidden = false;
  if (focus) {
    dom["error-banner"].focus({ preventScroll: true });
    dom["error-banner"].scrollIntoView({ behavior: "smooth", block: "center" });
  }
}

function hideError() {
  dom["error-banner"].hidden = true;
  dom["error-title"].textContent = "";
  dom["error-message"].textContent = "";
  dom["error-meta"].textContent = "";
}

function messageFromError(error) {
  return error && typeof error.message === "string" ? error.message : "알 수 없는 오류가 발생했습니다.";
}

function formatFrequency(value) {
  if (!Number.isFinite(value)) {
    return "—";
  }
  const absolute = Math.abs(value);
  if (absolute >= 1e9) {
    return `${formatNumber(value / 1e9)} GHz`;
  }
  if (absolute >= 1e6) {
    return `${formatNumber(value / 1e6)} MHz`;
  }
  if (absolute >= 1e3) {
    return `${formatNumber(value / 1e3)} kHz`;
  }
  return `${formatNumber(value)} Hz`;
}

function formatWithUnit(value, unit) {
  return `${formatNumber(value)} ${unit}`;
}

function formatNumber(value) {
  if (!Number.isFinite(value)) {
    return "—";
  }
  const absolute = Math.abs(value);
  if (absolute !== 0 && (absolute < 1e-4 || absolute >= 1e8)) {
    return value.toExponential(7).replace(/\.0+(?=e)/, "");
  }
  return new Intl.NumberFormat("ko-KR", {
    maximumSignificantDigits: 10,
    useGrouping: false,
  }).format(value);
}

function formatInteger(value) {
  return Number.isSafeInteger(value)
    ? new Intl.NumberFormat("ko-KR", { useGrouping: false }).format(value)
    : formatNumber(value);
}

function formatBuildId(value) {
  if (!Number.isSafeInteger(value) || value < 0) {
    return "—";
  }
  const hex = value.toString(16).padStart(8, "0");
  return `${value} · 0x${hex}`;
}

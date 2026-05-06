// Parser for .slog binary shot files
// Mirrors shot_log_format.h (keep in sync)
// Header: v4=128 bytes, v5+=512 bytes
// Dynamic field parsing based on fieldsMask for future extensibility

const HEADER_SIZE_V4 = 128;
const HEADER_SIZE_V5 = 512; // v5 and v6 share the 512-byte header (v6 fills reserved_v5 with controllerConfig + firmwareVersion)
const MAGIC = 0x544f4853; // 'SHOT' - matches backend SHOT_LOG_MAGIC

const TEMP_SCALE = 10;
const PRESSURE_SCALE = 10;
const FLOW_SCALE = 100;
const WEIGHT_SCALE = 10;
const RESISTANCE_SCALE = 100;
// `ho` (heater output) and `po` (pump output) both encode percent duty in
// hundredths-of-a-percent (firmware ×10 for ho's 0..1000 native range, ×100
// for po's 0..100 native range — both produce a 0..10000 stored range).
// Decoder rule: percent = stored / 100.0.
const DUTY_PERCENT_SCALE = 100;

// Field bit positions (must match shot_log_format.h)
const FIELD_BITS = {
  T: 0, // tick
  TT: 1, // target temp
  CT: 2, // current temp
  TP: 3, // target pressure
  CP: 4, // current pressure
  FL: 5, // pump flow
  TF: 6, // target flow
  PF: 7, // puck flow
  VF: 8, // volumetric flow
  V: 9, // volumetric weight
  EV: 10, // estimated weight
  PR: 11, // puck resistance
  SI: 12, // system info (v2+)
  // Phase number moved to header transitions in v5+
  HO: 13, // heater output (v6+)
  PO: 14, // pump output (v6+)
};

// Field definitions with parsing info
const FIELD_DEFS = {
  [FIELD_BITS.T]: {
    name: 't',
    type: 'uint16',
    scale: null,
    transform: (val, sampleInterval) => val * sampleInterval,
  },
  [FIELD_BITS.TT]: { name: 'tt', type: 'uint16', scale: TEMP_SCALE },
  [FIELD_BITS.CT]: { name: 'ct', type: 'uint16', scale: TEMP_SCALE },
  [FIELD_BITS.TP]: { name: 'tp', type: 'uint16', scale: PRESSURE_SCALE },
  [FIELD_BITS.CP]: { name: 'cp', type: 'uint16', scale: PRESSURE_SCALE },
  [FIELD_BITS.FL]: { name: 'fl', type: 'int16', scale: FLOW_SCALE },
  [FIELD_BITS.TF]: { name: 'tf', type: 'int16', scale: FLOW_SCALE },
  [FIELD_BITS.PF]: { name: 'pf', type: 'int16', scale: FLOW_SCALE },
  [FIELD_BITS.VF]: { name: 'vf', type: 'int16', scale: FLOW_SCALE },
  [FIELD_BITS.V]: { name: 'v', type: 'uint16', scale: WEIGHT_SCALE },
  [FIELD_BITS.EV]: { name: 'ev', type: 'uint16', scale: WEIGHT_SCALE },
  [FIELD_BITS.PR]: { name: 'pr', type: 'uint16', scale: RESISTANCE_SCALE },
  [FIELD_BITS.SI]: {
    name: 'systemInfo',
    type: 'uint16',
    scale: null,
    transform: val => ({
      raw: val,
      shotStartedVolumetric: !!(val & 0x0001),
      currentlyVolumetric: !!(val & 0x0002),
      bluetoothScaleConnected: !!(val & 0x0004),
      volumetricAvailable: !!(val & 0x0008),
      extendedRecording: !!(val & 0x0010),
    }),
  },
  // Phase number field removed in v5+, moved to header transitions
  [FIELD_BITS.HO]: { name: 'ho', type: 'uint16', scale: DUTY_PERCENT_SCALE },
  [FIELD_BITS.PO]: { name: 'po', type: 'uint16', scale: DUTY_PERCENT_SCALE },
};

function decodeCString(bytes) {
  let out = '';
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] === 0) break;
    out += String.fromCharCode(bytes[i]);
  }
  return out;
}

function countSetBits(n) {
  let count = 0;
  while (n) {
    count += n & 1;
    n >>= 1;
  }
  return count;
}

// Parse phase transitions from v5+ headers
function parsePhaseTransitions(view, transitionCount) {
  const transitions = [];
  const baseOffset = 110; // Offset after existing header fields

  for (let i = 0; i < transitionCount && i < 12; i++) {
    const offset = baseOffset + i * 29; // Each PhaseTransition is 29 bytes

    const sampleIndex = view.getUint16(offset, true);
    const phaseNumber = view.getUint8(offset + 2);
    // Skip reserved byte at offset + 3
    const phaseNameBytes = new Uint8Array(view.buffer, view.byteOffset + offset + 4, 25);
    const phaseName = decodeCString(phaseNameBytes);

    transitions.push({
      sampleIndex,
      phaseNumber,
      phaseName,
    });
  }

  return transitions;
}

export function parseBinaryShot(arrayBuffer, id) {
  const view = new DataView(arrayBuffer);

  // Read basic header info first
  if (view.byteLength < 16) throw new Error('File too small for header');

  const magic = view.getUint32(0, true);
  if (magic !== MAGIC)
    throw new Error(`Bad magic: expected 0x${MAGIC.toString(16)}, got 0x${magic.toString(16)}`);

  const version = view.getUint8(4);
  const deviceSampleSize = view.getUint8(5); // reserved0 holds sample size
  const headerSize = view.getUint16(6, true);

  // Determine expected header size based on version
  let expectedHeaderSize;
  if (version <= 4) {
    expectedHeaderSize = HEADER_SIZE_V4;
  } else {
    expectedHeaderSize = HEADER_SIZE_V5;
  }

  if (view.byteLength < expectedHeaderSize) {
    throw new Error(
      `File too small for v${version} header: need ${expectedHeaderSize} bytes, got ${view.byteLength}`,
    );
  }

  // Validate header size matches version
  if (headerSize !== expectedHeaderSize) {
    throw new Error(
      `Header size mismatch for v${version}: expected ${expectedHeaderSize}, got ${headerSize}`,
    );
  }

  // Parse common header fields
  const sampleInterval = view.getUint16(8, true);
  const fieldsMask = view.getUint32(12, true);
  const sampleCountHeader = view.getUint32(16, true);
  const durationHeader = view.getUint32(20, true);
  const startEpoch = view.getUint32(24, true);
  const profileIdBytes = new Uint8Array(arrayBuffer, 28, 32);
  const profileNameBytes = new Uint8Array(arrayBuffer, 60, 48);
  const finalWeightHeader = view.getUint16(108, true);
  const profileId = decodeCString(profileIdBytes);
  const profileName = decodeCString(profileNameBytes);

  // Parse phase transitions for v5+
  let phaseTransitions = [];
  if (version >= 5) {
    const transitionCount = view.getUint8(110 + 12 * 29); // After 12 PhaseTransitions
    phaseTransitions = parsePhaseTransitions(view, transitionCount);
  }

  // Parse v6+ controller-config and firmware-version blocks. These live in
  // what was reserved_v5 in earlier formats, starting immediately after
  // phaseTransitionCount (offset 459). Each ShotLogControllerConfig is 36 B
  // (9 floats), followed by ShotLogFirmwareVersion (4 bytes).
  let controllerConfig = null;
  let firmwareVersion = null;
  if (version >= 6) {
    const cfgOffset = 459;
    controllerConfig = {
      heaterKp: view.getFloat32(cfgOffset, true),
      heaterKi: view.getFloat32(cfgOffset + 4, true),
      heaterKd: view.getFloat32(cfgOffset + 8, true),
      heaterKff: view.getFloat32(cfgOffset + 12, true),
      pumpCoeffA: view.getFloat32(cfgOffset + 16, true),
      pumpCoeffB: view.getFloat32(cfgOffset + 20, true),
      pumpCoeffC: view.getFloat32(cfgOffset + 24, true),
      pumpCoeffD: view.getFloat32(cfgOffset + 28, true),
      idleHeaterOutput: view.getFloat32(cfgOffset + 32, true),
    };
    const fwOffset = cfgOffset + 36;
    firmwareVersion = {
      major: view.getUint8(fwOffset),
      minor: view.getUint8(fwOffset + 1),
      patch: view.getUint8(fwOffset + 2),
      commitsAhead: view.getUint8(fwOffset + 3),
    };
  }

  // Calculate expected sample size from fieldsMask
  const fieldCount = countSetBits(fieldsMask);
  const expectedSampleSize = fieldCount * 2; // Each field is 16 bits = 2 bytes

  if (deviceSampleSize !== expectedSampleSize) {
    throw new Error(
      `Field mask indicates ${fieldCount} fields (${expectedSampleSize} bytes), but device reports ${deviceSampleSize} bytes`,
    );
  }

  // Build field layout based on mask (preserves field order)
  const fieldLayout = [];
  for (let bitPos = 0; bitPos < 32; bitPos++) {
    if (fieldsMask & (1 << bitPos)) {
      const fieldDef = FIELD_DEFS[bitPos];
      if (fieldDef) {
        fieldLayout.push({ ...fieldDef, bitPos });
      } else {
        // Unknown field - skip but track position
        const fieldSize = 2; // assume uint16 for unknown fields
        fieldLayout.push({
          name: `unknown_${bitPos}`,
          type: 'uint16',
          scale: null,
          bitPos,
          size: fieldSize,
        });
      }
    }
  }

  const samples = [];
  const dataBytes = view.byteLength - headerSize;
  if (dataBytes < 0) {
    throw new Error('Data size misaligned');
  }
  const sampleSize = deviceSampleSize;
  const fullSampleBytes = Math.floor(dataBytes / sampleSize) * sampleSize;
  const trailingBytes = dataBytes - fullSampleBytes;
  const inferredSamples = fullSampleBytes / sampleSize;
  const maxSamples = sampleCountHeader
    ? Math.min(sampleCountHeader, inferredSamples)
    : inferredSamples;

  for (let i = 0; i < maxSamples; i++) {
    const base = headerSize + i * sampleSize;
    const sample = {};

    // Parse each field dynamically
    for (let fieldIdx = 0; fieldIdx < fieldLayout.length; fieldIdx++) {
      const field = fieldLayout[fieldIdx];
      const offset = base + fieldIdx * 2; // Each field is 2 bytes

      let rawValue;
      if (field.type === 'int16') {
        rawValue = view.getInt16(offset, true);
      } else {
        rawValue = view.getUint16(offset, true);
      }

      let finalValue;
      if (field.transform) {
        finalValue = field.transform(rawValue, sampleInterval);
      } else if (field.scale) {
        finalValue = rawValue / field.scale;
      } else {
        finalValue = rawValue;
      }

      sample[field.name] = finalValue;
    }

    // For v5+ files, reconstruct phase information from transitions
    if (version >= 5) {
      // Find the current phase for this sample
      let currentPhase = 0;
      let phaseName = 'Phase 1';

      for (let t = 0; t < phaseTransitions.length; t++) {
        if (i >= phaseTransitions[t].sampleIndex) {
          currentPhase = phaseTransitions[t].phaseNumber;
          phaseName = phaseTransitions[t].phaseName;
        } else {
          break;
        }
      }

      sample.phaseNumber = currentPhase;
      sample.phaseDisplayNumber = currentPhase + 1; // 1-based for display
      sample.phaseName = phaseName;
    }

    samples.push(sample);
  }

  const lastT = samples.length ? samples[samples.length - 1].t : 0;
  const headerIncomplete = sampleCountHeader === 0;
  const inferredIncomplete =
    trailingBytes !== 0 || (sampleCountHeader && sampleCountHeader > inferredSamples);
  const incomplete = headerIncomplete || inferredIncomplete;
  const effectiveDuration = !incomplete && durationHeader ? durationHeader : lastT;

  const headerVolume = finalWeightHeader ? finalWeightHeader / WEIGHT_SCALE : 0;
  const sampleVolume = samples.length ? samples[samples.length - 1].v : 0;
  const volume = headerVolume > 0 ? headerVolume : sampleVolume > 0 ? sampleVolume : null;

  return {
    id,
    version,
    profile: profileName,
    profileId,
    timestamp: startEpoch,
    duration: effectiveDuration,
    samples,
    volume,
    incomplete,
    sampleInterval,
    fieldsMask,
    trailingBytes,
    samplesExpected: sampleCountHeader,
    phaseTransitions, // v5+ phase transition data
    controllerConfig, // v6+ PID gains, pump coeffs, idle heater output (null for older files)
    firmwareVersion, // v6+ {major, minor, patch, commitsAhead} (null for older files)
  };
}

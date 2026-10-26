// GENERATED from common/amv_dongle_proto.h by firmware/esp32p4-dongle/tools/gen_proto_js.py. Do not edit.
export const P = {
  CMD: {
    GET_INFO: 1,
    GET_WEIGHTS: 2,
    SET_WEIGHTS: 3,
    SAVE_PROFILE: 4,
    LOAD_PROFILE: 5,
    SET_BYPASS: 6,
    GET_STATS: 7,
    TEST_SCALE: 8,
    TEST_SELF_CHANGE: 9,
    REBOOT: 10,
    FACTORY_RESET: 11,
    GET_SLOTS: 12,
    HELLO: 13,
    GET_MOUSE: 14,
    GET_MOUSE_CAPS: 15,
    GET_MOUSE_DESC: 16,
    SET_MOUSE_CPI: 17,
    SET_MOUSE_POLLING: 18,
    EVENT_STATE: 128,
    EVENT_MOUSE: 129,
  },
  ST: {
    OK: 0,
    BAD_CMD: 1,
    BAD_LEN: 2,
    BAD_VALUE: 3,
    STALE: 4,
    BAD_SLOT: 5,
    STORE_ERR: 6,
    BUSY: 7,
    EMPTY: 8,
    BAD_MAGIC: 9,
    NO_MOUSE: 10,
    UNSUPPORTED: 11,
    DEVICE_ERR: 12,
  },
  SRC: {
    HOST: 0,
    DEVICE: 1,
    LOAD: 2,
    BOOT: 3,
  },
  TIER: {
    KNOWN: 1,
    GENERIC: 2,
    BOOT: 3,
    IGNORED: 4,
  },
  MF: {
    REPORT_IDS: 1,
    WHEEL: 2,
    PAN: 4,
    ALSO_KEYBOARD: 8,
    VERIFIED: 16,
    CAN_SET_CPI: 32,
    CAN_SET_RATE: 64,
    DRV_ERROR: 128,
  },
  CPI: {
    NONE: 0,
    USER: 1,
    DEVICE: 2,
  },
  DRV: {
    NONE: 0,
    RAZER: 1,
    FINALMOUSE: 2,
    GEARHUB: 3,
  },
  IGN: {
    NOT_MOUSE: 1,
    ABSOLUTE: 2,
    UNPARSED: 3,
    NO_SLOT: 4,
    CLAIM: 5,
  },
  MEV: {
    ATTACHED: 1,
    REMOVED: 2,
    UPDATED: 3,
  },
  PROTO_VERSION: 1,
  FRAME_SIZE: 64,
  HEADER_SIZE: 8,
  PAYLOAD_MAX: 56,
  MAGIC0: 65,
  MAGIC1: 77,
  VENDOR_USAGE_PAGE: 65357,
  VENDOR_USAGE: 1,
  NUM_SLOTS: 4,
  USB_VID: 12346,
  USB_PID: 19798,
  WEIGHT_MIN: 0.01,
  WEIGHT_MAX: 5.0,
  GEN_ANY: 4294967295,
  HELLO_IDENT: 1146506561,
  HELLO_NONCE_LEN: 8,
  CAP_VECTORED_MOUSE: 1,
  CAP_MOUSE_HOST: 2,
  TEST_RESET_REM: 1,
};

export const CMD_NAMES = { 1: 'GET_INFO', 2: 'GET_WEIGHTS', 3: 'SET_WEIGHTS', 4: 'SAVE_PROFILE', 5: 'LOAD_PROFILE', 6: 'SET_BYPASS', 7: 'GET_STATS', 8: 'TEST_SCALE', 9: 'TEST_SELF_CHANGE', 10: 'REBOOT', 11: 'FACTORY_RESET', 12: 'GET_SLOTS', 13: 'HELLO', 14: 'GET_MOUSE', 15: 'GET_MOUSE_CAPS', 16: 'GET_MOUSE_DESC', 17: 'SET_MOUSE_CPI', 18: 'SET_MOUSE_POLLING', 128: 'EVENT_STATE', 129: 'EVENT_MOUSE' };
export const ST_NAMES = { 0: 'OK', 1: 'BAD_CMD', 2: 'BAD_LEN', 3: 'BAD_VALUE', 4: 'STALE', 5: 'BAD_SLOT', 6: 'STORE_ERR', 7: 'BUSY', 8: 'EMPTY', 9: 'BAD_MAGIC', 10: 'NO_MOUSE', 11: 'UNSUPPORTED', 12: 'DEVICE_ERR' };
export const SRC_NAMES = { 0: 'HOST', 1: 'DEVICE', 2: 'LOAD', 3: 'BOOT' };
export const TIER_NAMES = { 1: 'KNOWN', 2: 'GENERIC', 3: 'BOOT', 4: 'IGNORED' };
export const MF_NAMES = { 1: 'REPORT_IDS', 2: 'WHEEL', 4: 'PAN', 8: 'ALSO_KEYBOARD', 16: 'VERIFIED', 32: 'CAN_SET_CPI', 64: 'CAN_SET_RATE', 128: 'DRV_ERROR' };
export const CPI_NAMES = { 0: 'NONE', 1: 'USER', 2: 'DEVICE' };
export const DRV_NAMES = { 0: 'NONE', 1: 'RAZER', 2: 'FINALMOUSE', 3: 'GEARHUB' };
export const IGN_NAMES = { 1: 'NOT_MOUSE', 2: 'ABSOLUTE', 3: 'UNPARSED', 4: 'NO_SLOT', 5: 'CLAIM' };
export const MEV_NAMES = { 1: 'ATTACHED', 2: 'REMOVED', 3: 'UPDATED' };

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <cmath>
#include <limits>

#include "amv_store_teensy.h"

// Ensure static assert requirement holds
static_assert(sizeof(amv_store_layout_t) <= AMV_STORE_EEPROM_SIZE,
              "amv_store_layout_t must fit in 4284 bytes");

static uint8_t s_test_ram[AMV_STORE_EEPROM_SIZE];
static unsigned s_read_count = 0;
static unsigned s_write_count = 0;

static bool test_ram_read(uint16_t addr, void *buf, size_t n) {
    if ((size_t)addr + n > sizeof(s_test_ram)) return false;
    memcpy(buf, &s_test_ram[addr], n);
    s_read_count++;
    return true;
}

static bool test_ram_write(uint16_t addr, const void *buf, size_t n) {
    if ((size_t)addr + n > sizeof(s_test_ram)) return false;
    memcpy(&s_test_ram[addr], buf, n);
    s_write_count++;
    return true;
}

static const amv_store_backend_t s_test_backend = {
    test_ram_read,
    test_ram_write,
};

static void reset_test_env(void) {
    memset(s_test_ram, 0, sizeof(s_test_ram));
    s_read_count = 0;
    s_write_count = 0;
    amv_store_set_backend(&s_test_backend);
}

static void test_ops_presence(void) {
    const amv_store_ops_t *ops = amv_store_teensy_ops();
    assert(ops != nullptr);
    assert(ops->load != nullptr);
    assert(ops->save != nullptr);
    assert(ops->valid_mask != nullptr);
    assert(ops->factory_reset != nullptr);
    assert(ops->load_boot != nullptr);
    assert(ops->save_boot != nullptr);
    printf("  [PASS] test_ops_presence\n");
}

static void test_slot_round_trip(void) {
    reset_test_env();
    const amv_store_ops_t *ops = amv_store_teensy_ops();

    for (uint8_t slot = 0; slot < AMV_NUM_SLOTS; slot++) {
        amv_profile_t p_save;
        for (int i = 0; i < 8; i++) {
            p_save.w[i] = 1.0f + 0.5f * slot + 0.1f * i;
        }
        assert(ops->save(slot, &p_save));

        amv_profile_t p_load;
        memset(&p_load, 0, sizeof(p_load));
        assert(ops->load(slot, &p_load));

        for (int i = 0; i < 8; i++) {
            assert(p_load.w[i] == p_save.w[i]);
        }
    }

    // Out of bounds and null pointer tests
    amv_profile_t dummy;
    assert(!ops->save(AMV_NUM_SLOTS, &dummy));
    assert(!ops->load(AMV_NUM_SLOTS, &dummy));
    assert(!ops->save(255, &dummy));
    assert(!ops->load(255, &dummy));
    assert(!ops->save(0, nullptr));
    assert(!ops->load(0, nullptr));

    printf("  [PASS] test_slot_round_trip\n");
}

static void test_valid_mask(void) {
    reset_test_env();
    const amv_store_ops_t *ops = amv_store_teensy_ops();

    assert(ops->valid_mask() == 0);

    amv_profile_t p;
    for (int i = 0; i < 8; i++) p.w[i] = 1.0f;

    assert(ops->save(0, &p));
    assert(ops->valid_mask() == 0x01);

    assert(ops->save(2, &p));
    assert(ops->valid_mask() == 0x05); // bits 0 and 2

    assert(ops->save(1, &p));
    assert(ops->valid_mask() == 0x07); // bits 0, 1, 2

    assert(ops->save(3, &p));
    assert(ops->valid_mask() == 0x0F); // all 4 slots

    printf("  [PASS] test_valid_mask\n");
}

static void test_crc_corruption(void) {
    reset_test_env();
    const amv_store_ops_t *ops = amv_store_teensy_ops();

    amv_profile_t p_in;
    for (int i = 0; i < 8; i++) p_in.w[i] = 2.0f;
    assert(ops->save(1, &p_in));
    assert(ops->valid_mask() == 0x02);

    // Corrupt one byte inside slot 1 payload
    uint16_t slot1_offset = (uint16_t)(1 * sizeof(amv_slot_record_t));
    s_test_ram[slot1_offset + 10] ^= 0x55;

    // Loading corrupted slot must fail and NOT overwrite with garbage weights
    amv_profile_t p_out;
    for (int i = 0; i < 8; i++) p_out.w[i] = 99.0f; // Sentinel
    assert(!ops->load(1, &p_out));
    // Sentinel remains untouched
    assert(p_out.w[0] == 99.0f);

    // Corrupted slot must not appear in valid_mask
    assert(ops->valid_mask() == 0x00);

    printf("  [PASS] test_crc_corruption\n");
}

static void test_invalid_weights(void) {
    reset_test_env();
    const amv_store_ops_t *ops = amv_store_teensy_ops();

    // 1. Direct save rejection
    amv_profile_t p_invalid;
    for (int i = 0; i < 8; i++) p_invalid.w[i] = 1.0f;
    p_invalid.w[3] = 0.005f; // below AMV_WEIGHT_MIN (0.01)
    assert(!ops->save(0, &p_invalid));

    p_invalid.w[3] = 5.05f; // above AMV_WEIGHT_MAX (5.0)
    assert(!ops->save(0, &p_invalid));

    p_invalid.w[3] = std::numeric_limits<float>::quiet_NaN();
    assert(!ops->save(0, &p_invalid));

    p_invalid.w[3] = std::numeric_limits<float>::infinity();
    assert(!ops->save(0, &p_invalid));

    // 2. Reject invalid weights on load even if CRC matches
    amv_slot_record_t rec;
    rec.magic = AMV_STORE_SLOT_MAGIC;
    rec.schema = AMV_STORE_SCHEMA;
    rec.reserved = 0;
    for (int i = 0; i < 8; i++) rec.w[i] = 1.0f;
    rec.w[4] = 0.009f; // invalid (< 0.01)
    rec.crc = amv_store_crc32(&rec, sizeof(rec) - sizeof(rec.crc));

    // Manually write record to slot 2
    uint16_t slot2_addr = (uint16_t)(2 * sizeof(amv_slot_record_t));
    s_test_backend.write(slot2_addr, &rec, sizeof(rec));

    amv_profile_t p_out;
    for (int i = 0; i < 8; i++) p_out.w[i] = 42.0f;
    assert(!ops->load(2, &p_out));
    assert(p_out.w[0] == 42.0f); // Untouched
    assert((ops->valid_mask() & (1u << 2)) == 0);

    printf("  [PASS] test_invalid_weights\n");
}

static void test_factory_reset(void) {
    reset_test_env();
    const amv_store_ops_t *ops = amv_store_teensy_ops();

    // Populate all slots and boot record
    amv_profile_t p;
    for (int i = 0; i < 8; i++) p.w[i] = 1.5f;

    for (uint8_t i = 0; i < AMV_NUM_SLOTS; i++) {
        assert(ops->save(i, &p));
    }
    assert(ops->save_boot(2, &p, true));

    assert(ops->valid_mask() == 0x0F);

    uint8_t boot_slot = 0xFF;
    amv_profile_t boot_p;
    assert(ops->load_boot(&boot_slot, &boot_p));
    assert(boot_slot == 2);
    assert(amv_store_boot_bypass() == true);

    // Perform factory reset
    assert(ops->factory_reset());

    // Valid mask should now be completely 0
    assert(ops->valid_mask() == 0);

    // All slots should fail to load
    for (uint8_t i = 0; i < AMV_NUM_SLOTS; i++) {
        assert(!ops->load(i, &p));
    }

    // Boot record should fail to load
    boot_slot = 0xFF;
    assert(!ops->load_boot(&boot_slot, &boot_p));

    printf("  [PASS] test_factory_reset\n");
}

static void test_boot_round_trip(void) {
    reset_test_env();
    const amv_store_ops_t *ops = amv_store_teensy_ops();

    amv_profile_t p_boot_save;
    for (int i = 0; i < 8; i++) p_boot_save.w[i] = 0.5f + 0.2f * i;

    // Test with active_slot 3 and bypass true
    assert(ops->save_boot(3, &p_boot_save, true));

    uint8_t active_slot = 0xFF;
    amv_profile_t p_boot_load;
    memset(&p_boot_load, 0, sizeof(p_boot_load));
    assert(ops->load_boot(&active_slot, &p_boot_load));
    assert(active_slot == 3);
    assert(amv_store_boot_bypass() == true);
    for (int i = 0; i < 8; i++) {
        assert(p_boot_load.w[i] == p_boot_save.w[i]);
    }

    // Test load_boot_full helper
    bool bypass_read = false;
    uint8_t slot_full = 0xFF;
    amv_profile_t p_full;
    assert(amv_store_load_boot_full(&slot_full, &p_full, &bypass_read));
    assert(slot_full == 3);
    assert(bypass_read == true);

    // Test with active_slot 1 and bypass false
    for (int i = 0; i < 8; i++) p_boot_save.w[i] = 2.0f;
    assert(ops->save_boot(1, &p_boot_save, false));
    assert(ops->load_boot(&active_slot, &p_boot_load));
    assert(active_slot == 1);
    assert(amv_store_boot_bypass() == false);
    for (int i = 0; i < 8; i++) {
        assert(p_boot_load.w[i] == 2.0f);
    }

    // Test boot record CRC corruption
    uint16_t boot_addr = (uint16_t)offsetof(amv_store_layout_t, boot);
    s_test_ram[boot_addr + 8] ^= 0xFF;
    assert(!ops->load_boot(&active_slot, &p_boot_load));

    // Test boot record invalid weights rejection
    assert(!ops->save_boot(1, nullptr, false));
    p_boot_save.w[0] = 10.0f; // out of range
    assert(!ops->save_boot(1, &p_boot_save, false));

    // Test boot record invalid active_slot
    p_boot_save.w[0] = 1.0f;
    assert(!ops->save_boot(AMV_NUM_SLOTS, &p_boot_save, false));

    printf("  [PASS] test_boot_round_trip\n");
}

static void test_cpi_labels(void) {
    printf("  [TEST] test_cpi_labels\n");
    reset_test_env();
    uint16_t cpi = 0;
    assert(!amv_store_label_get(0x1234, 0x5678, &cpi));
    assert(amv_store_label_set(0x1234, 0x5678, 1600));
    assert(amv_store_label_set(0x046d, 0xc08b, 800));
    assert(amv_store_label_get(0x1234, 0x5678, &cpi) && cpi == 1600);
    assert(amv_store_label_set(0x1234, 0x5678, 3200));                 // overwrite in place
    assert(amv_store_label_get(0x1234, 0x5678, &cpi) && cpi == 3200);
    assert(amv_store_label_get(0x046d, 0xc08b, &cpi) && cpi == 800);
    // the labels live after the profiles and the boot record and never disturb them
    amv_profile_t p; for (int i = 0; i < 8; i++) p.w[i] = 1.5f;
    const amv_store_ops_t *ops = amv_store_teensy_ops();
    assert(ops->save(3, &p) && ops->save_boot(3, &p, false));
    assert(amv_store_label_get(0x1234, 0x5678, &cpi) && cpi == 3200);
    assert(amv_store_label_set(0x1234, 0x5678, 0));                    // 0 removes
    assert(!amv_store_label_get(0x1234, 0x5678, &cpi));
    for (uint16_t i = 0; i < AMV_STORE_LABELS + 3; i++) assert(amv_store_label_set(0x1000, i, (uint16_t)(400 + i)));   // full: replaces
    assert(amv_store_label_get(0x1000, AMV_STORE_LABELS + 2, &cpi) && cpi == 400 + AMV_STORE_LABELS + 2);
    // a corrupted record is ignored
    uint16_t off = (uint16_t)offsetof(amv_store_layout_t, labels);
    for (int i = 0; i < AMV_STORE_LABELS; i++) s_test_ram[off + i * sizeof(amv_label_record_t) + 6] ^= 0x55;
    assert(!amv_store_label_get(0x046d, 0xc08b, &cpi));
    amv_profile_t q; assert(ops->load(3, &q) && q.w[0] == 1.5f);
    assert(ops->factory_reset());
    assert(!amv_store_label_get(0x1000, AMV_STORE_LABELS + 2, &cpi));
    printf("  [PASS] test_cpi_labels\n");
}

int main(void) {
    printf("Running amv_store_teensy unit tests...\n");
    test_ops_presence();
    test_slot_round_trip();
    test_valid_mask();
    test_crc_corruption();
    test_invalid_weights();
    test_factory_reset();
    test_boot_round_trip();
    test_cpi_labels();
    printf("All amv_store_teensy tests passed!\n");
    return 0;
}

#define CMD_GET_FW_VERSION		0xb0
#define CMD_START			0xb1
#define CMD_GET_REVID_VERSION		0xb2
#define CMD_STOP			0xb3
#define CMD_GET_ACQ_STATUS		0xb4
#define CMD_RESET			0xb5
#define CMD_SET_ULPI_MODE		0xb6
#define CMD_GET_ULPI_STATUS		0xb7
#define CMD_GET_ULPI_PIN_STATUS		0xb8
#define CMD_GET_ULPI_ACQ_STATUS		0xb9
#define CMD_SET_ULPI_PHASE		0xba

#define ULPI_MODE_CONFIG_FORCE		1
#define ULPI_CORE_PHASE_MAX		15
#define ULPI_CORE_PHASE_DLL_OFF		16
#define ULPI_DLL_CONFIG_PHASE		0
#define ULPI_DLL_CONFIG_FIXED_DELAY	1
#define ULPI_DLL_FIXED_DELAY_MAX	1023

#define ACQ_STATUS_FLAGS_STOP_PENDING	(1 << 0)

#define ULPI_STATUS_READ_VENDOR_ID_LOW		(1 << 0)
#define ULPI_STATUS_READ_VENDOR_ID_HIGH		(1 << 1)
#define ULPI_STATUS_READ_PRODUCT_ID_LOW		(1 << 2)
#define ULPI_STATUS_READ_PRODUCT_ID_HIGH		(1 << 3)
#define ULPI_STATUS_READ_FUNCTION_CONTROL	(1 << 4)
#define ULPI_STATUS_READ_INTERFACE_CONTROL	(1 << 5)
#define ULPI_STATUS_READ_OTG_CONTROL		(1 << 6)
#define ULPI_STATUS_READ_DEBUG			(1 << 7)
#define ULPI_STATUS_READ_SCRATCH_BEFORE		(1 << 8)
#define ULPI_STATUS_READ_SCRATCH_AFTER		(1 << 9)
#define ULPI_STATUS_READ_SCRATCH_RESTORED	(1 << 10)
#define ULPI_STATUS_READ_ALL			((1 << 11) - 1)

#define ULPI_STATUS_WRITE_FUNCTION_CONTROL	(1 << 0)
#define ULPI_STATUS_WRITE_OTG_CONTROL		(1 << 1)
#define ULPI_STATUS_WRITE_SCRATCH_TEST		(1 << 2)
#define ULPI_STATUS_WRITE_SCRATCH_RESTORE	(1 << 3)
#define ULPI_STATUS_WRITE_ALL			((1 << 4) - 1)

#define ULPI_STATUS_MATCH_VENDOR_ID_LOW		(1 << 0)
#define ULPI_STATUS_MATCH_VENDOR_ID_HIGH		(1 << 1)
#define ULPI_STATUS_MATCH_PRODUCT_ID_LOW		(1 << 2)
#define ULPI_STATUS_MATCH_PRODUCT_ID_HIGH	(1 << 3)
#define ULPI_STATUS_MATCH_FUNCTION_CONTROL	(1 << 4)
#define ULPI_STATUS_MATCH_INTERFACE_CONTROL	(1 << 5)
#define ULPI_STATUS_MATCH_OTG_CONTROL		(1 << 6)
#define ULPI_STATUS_MATCH_SCRATCH		(1 << 7)
#define ULPI_STATUS_MATCH_ALL			0xff

#define CMD_START_FLAGS_SUPERWIDE_POS   3
#define CMD_START_FLAGS_CLK_CTL2_POS    4
#define CMD_START_FLAGS_WIDE_POS        5
#define CMD_START_FLAGS_CLK_SRC_POS     6

#define CMD_START_FLAGS_EXT_CLOCK       (1 << 0)
#define CMD_START_FLAGS_CLK_INVERT      (1 << 1)
#define CMD_START_FLAGS_CLK_CTL2        (1 << CMD_START_FLAGS_CLK_CTL2_POS)
#define CMD_START_FLAGS_SAMPLE_8BIT     (0 << CMD_START_FLAGS_WIDE_POS)
#define CMD_START_FLAGS_SAMPLE_16BIT    (1 << CMD_START_FLAGS_WIDE_POS)
#define CMD_START_FLAGS_SAMPLE_24BIT    ((0 << CMD_START_FLAGS_WIDE_POS) | (1 << CMD_START_FLAGS_SUPERWIDE_POS))
#define CMD_START_FLAGS_SAMPLE_32BIT    ((1 << CMD_START_FLAGS_WIDE_POS) | (1 << CMD_START_FLAGS_SUPERWIDE_POS))

#define CMD_START_FLAGS_CLK_30MHZ       (0 << CMD_START_FLAGS_CLK_SRC_POS)
#define CMD_START_FLAGS_CLK_48MHZ       (1 << CMD_START_FLAGS_CLK_SRC_POS)
#define CMD_START_FLAGS_CLK_192MHZ      (2 << CMD_START_FLAGS_CLK_SRC_POS)
#define CMD_START_FLAGS_CLK_80MHZ       (3 << CMD_START_FLAGS_CLK_SRC_POS)
#define CMD_START_FLAGS_CLK_89MHZ       (CMD_START_FLAGS_CLK_CTL2 | CMD_START_FLAGS_CLK_192MHZ)
#define CMD_START_FLAGS_CLK_SRC_MASK    (3 << CMD_START_FLAGS_CLK_SRC_POS)

struct version_info {
        uint8_t major;
        uint8_t minor;
};

struct cmd_start_acquisition {
        uint8_t flags;
        uint8_t sample_delay_h;
        uint8_t sample_delay_l;
};

struct acquisition_status {
        uint8_t gpif_stat;
        uint8_t gpif_state;
        uint16_t reserved;
        uint32_t gpif_status;
        uint32_t gpif_intr;
        uint32_t pib_intr;
        uint32_t pib_error;
        uint32_t pib_sck0_status;
        uint32_t pib_sck0_intr;
        uint32_t pib_sck0_dscr;
        uint32_t pib_sck0_count;
        uint32_t pib_sck1_status;
        uint32_t pib_sck1_intr;
        uint32_t pib_sck1_dscr;
        uint32_t pib_sck1_count;
        uint32_t uib_sck2_status;
        uint32_t uib_sck2_intr;
        uint32_t uib_sck2_dscr;
        uint32_t uib_sck2_count;
        uint32_t eepm_cs;
        uint32_t eepm_endpoint2;
        uint32_t prot_epi_cs1;
        uint32_t pause_count;
        uint8_t pause_gpif_stat;
        uint8_t pause_gpif_state;
        uint16_t pause_reserved;
        uint32_t pause_gpif_status;
        uint32_t pause_pib_sck0_status;
        uint32_t pause_pib_sck0_dscr;
        uint32_t pause_pib_sck0_count;
        uint32_t pause_pib_sck1_status;
        uint32_t pause_pib_sck1_dscr;
        uint32_t pause_pib_sck1_count;
        uint32_t pause_uib_sck2_status;
        uint32_t pause_uib_sck2_dscr;
        uint32_t pause_uib_sck2_count;
};

struct ulpi_status {
	uint8_t requested_speed;
	uint8_t expected_function_control;
	uint8_t vendor_id_low;
	uint8_t vendor_id_high;
	uint8_t product_id_low;
	uint8_t product_id_high;
	uint8_t function_control;
	uint8_t interface_control;
	uint8_t otg_control;
	uint8_t debug;
	uint8_t scratch_before;
	uint8_t scratch_expected;
	uint8_t scratch_after;
	uint8_t scratch_restored;
	uint16_t read_ok_mask;
	uint8_t write_ok_mask;
	uint8_t match_mask;
	uint8_t last_gpif_state;
	uint8_t last_gpif_stat;
	uint32_t last_gpif_status;
	uint32_t gpio_invalue0;
	uint32_t alpha_stat;
	uint32_t beta_stat;
	uint32_t data_ctrl;
	uint8_t write_gpif_state;
	uint8_t write_gpif_stat;
	uint16_t write_reserved;
	uint32_t write_gpio_invalue0;
	uint32_t write_alpha_stat;
	uint32_t write_beta_stat;
	uint32_t write_data_ctrl;
	uint32_t read_ingress_data;
	uint32_t gpio_simple_override0;
	uint32_t gpio8_config;
	uint32_t gpio9_config;
};

#define ULPI_PIN_SIGNAL_COUNT	6
#define ULPI_PIN_PAIR_COUNT	3

enum ulpi_pin_signal {
	ULPI_PIN_CTL2_DIR = 0,
	ULPI_PIN_DQ10_DIR,
	ULPI_PIN_CTL0_NXT,
	ULPI_PIN_DQ9_NXT,
	ULPI_PIN_CTL3_STP,
	ULPI_PIN_DQ8_STP,
};

enum ulpi_pin_pair {
	ULPI_PIN_PAIR_DIR = 0,
	ULPI_PIN_PAIR_NXT,
	ULPI_PIN_PAIR_STP,
};

struct ulpi_pin_status {
	uint32_t samples;
	uint32_t high[ULPI_PIN_SIGNAL_COUNT];
	uint32_t transitions[ULPI_PIN_SIGNAL_COUNT];
	uint32_t mismatches[ULPI_PIN_PAIR_COUNT];
	uint32_t dir_to_dq8_mismatches;
	uint32_t nxt_to_dq10_mismatches;
	uint32_t data_nonzero_samples;
	uint32_t data_transitions;
	uint32_t rx_samples;
	uint32_t rx_nonzero_samples;
	uint32_t rx_nxt_samples;
	uint32_t rx_data_or;
	uint32_t ingress_nonzero_samples;
	uint32_t ingress_transitions;
	uint32_t rx_ingress_nonzero_samples;
	uint32_t rx_ingress_or;
	uint32_t drive_state_samples;
	uint32_t receive_state_samples;
	uint32_t dq_oen_samples;
	uint32_t other_state_samples;
};

struct ulpi_acquisition_status {
	uint32_t poll_count;
	uint32_t dir_high_count;
	uint32_t sampling_state_count;
	uint32_t wait_state_count;
	uint32_t dir_data_nonzero_count;
	uint32_t dir_ingress_nonzero_count;
	uint32_t alpha_dq_oen_while_dir_count;
	uint32_t ingress_nonzero_count;
	uint32_t bus_config;
	uint32_t bus_config2;
	uint32_t ad_config;
	uint32_t ctrl_bus_direction;
	uint32_t ctrl_bus_polarity;
	uint32_t last_gpio_invalue0;
	uint32_t last_alpha_stat;
	uint32_t last_waveform_ctrl_stat;
	uint32_t last_ingress_data;
	uint32_t sample_base_word1;
	uint32_t sample_mirror_word1;
	uint32_t gpif_config;
	uint32_t pib_dll_ctrl;
	uint32_t core_phase;
	uint32_t dll_config;
};

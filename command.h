#define CMD_GET_FW_VERSION		0xb0
#define CMD_START			0xb1
#define CMD_GET_REVID_VERSION		0xb2
#define CMD_STOP			0xb3
#define CMD_GET_ACQ_STATUS		0xb4
#define CMD_RESET			0xb5

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

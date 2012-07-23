#ifndef __DISPSW_REM__
#define __DISPSW_REM__

#define MAX_OVERLAYS  (10)	/* A little bit of future proofing */
#define DISPSW_MR_MAX_RES_SUPPORTED     (20)

struct dispsw_vrfb {
        bool need_cfg;

        /* Ping-pong buffers and VRFB contexts */
        struct vrfb ctx[2];
        unsigned long paddr[2];
        void *vaddr[2];
        int size;
        int next;

        /* VRFB dma config data */
        u32 en;
        u32 fn;
        u32 dst_ei;
        u32 dst_fi;

        /* VRFB dma channel data */
        int dma_id;
        int dma_ch;
        bool dma_complete;
        wait_queue_head_t wait;
};

struct dispsw_rotate_data {
        struct mutex mtx; /* Lock for all device accesses */

        struct dispsw_vrfb vrfb;

        int max_w;
        int max_h;
        int max_buf_size;

        int buf_w;
        int buf_h;
        int buf_stride;
        int buf_rot;    /* 1 = 90; 2 = 180; 3 = 270; */
        enum omap_color_mode buf_fmt;
        int buf_offset;

        int dss_rot;    /* 1 = 90; 2 = 180; 3 = 270; */
};

struct dispsw_mr_data {
        struct mutex mtx; /* Lock for all device accesses */

        int num_entries;
        struct dispsw_mr_support *res[DISPSW_MR_MAX_RES_SUPPORTED];
};

enum dispsw_rotate {
        DISPSW_ROTATE_IGNORE,
        DISPSW_ROTATE_0,
        DISPSW_ROTATE_90,
        DISPSW_ROTATE_180,
        DISPSW_ROTATE_270,
};

enum dispsw_scale {
        DISPSW_SCALE_IGNORE,
        DISPSW_SCALE_FIT_TO_SCREEN,
        DISPSW_SCALE_PERCENT, /* Percent of increase or decrease of plane */
};

enum dispsw_align {
        DISPSW_ALIGN_IGNORE,
        DISPSW_ALIGN_CENTER,
        DISPSW_ALIGN_TOP,
        DISPSW_ALIGN_TOP_LEFT,
        DISPSW_ALIGN_TOP_CENTER,
        DISPSW_ALIGN_TOP_RIGHT,
        DISPSW_ALIGN_BOTTOM,
        DISPSW_ALIGN_BOTTOM_LEFT,
        DISPSW_ALIGN_BOTTOM_CENTER,
        DISPSW_ALIGN_BOTTOM_RIGHT,
        DISPSW_ALIGN_LEFT,
        DISPSW_ALIGN_LEFT_CENTER,
        DISPSW_ALIGN_RIGHT,
        DISPSW_ALIGN_RIGHT_CENTER,
        DISPSW_ALIGN_PERCENT,  /* Percent of display size to align to.
                                * Top-left of display to top-left of plane
                                */
};

typedef int (*ovl_set_info)(struct omap_overlay *ovl,
                                struct omap_overlay_info *info);
typedef void (*ovl_get_info)(struct omap_overlay *ovl,
                                struct omap_overlay_info *info);

struct dispsw_osi {
        int id;
        int idx;

        ovl_set_info set_func;
        ovl_get_info get_func;
        struct omap_overlay *ovl;

        struct omap_overlay_info last_info;

        bool override;
        bool persist;

        struct omap_dss_device *dssdev;
        int disp_w;
        int disp_h;

        bool force_disabled;
        bool stored_enable;
        int  force_cnt;

        bool lock_aspect_ratio;
        enum dispsw_rotate rotate;
        enum dispsw_scale scale;
        int v_scale_percent;
        int h_scale_percent;
        enum dispsw_align align;
        int v_align_percent;
        int h_align_percent;
};


struct dispsw_device {
        struct mutex  mtx; /* Lock for all device accesses */

        int major;
        struct class *cls;
        struct device *dev;

        int opened;

        struct dispsw_rotate_data rot;
        struct dispsw_mr_data mr;

        int num_ovls;

        /*
         * Performance hack to turn off GFX plane for playback
         * played to an overridden device
         */
        int videoOverrideEnabled;

        /* Data, per overlay, for overriding the overlay set info call */
        struct dispsw_osi osi[MAX_OVERLAYS];

        bool no_update;
};

#endif

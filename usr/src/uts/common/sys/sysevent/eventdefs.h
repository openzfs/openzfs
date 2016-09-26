/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2015 Nexenta Systems, Inc.  All rights reserved.
 */

#ifndef	_SYS_SYSEVENT_EVENTDEFS_H
#define	_SYS_SYSEVENT_EVENTDEFS_H

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * eventdefs.h contains public definitions for sysevent types (classes
 * and subclasses).  All additions/removal/changes are subject
 * to PSARC approval.
 */

/* Sysevent Class definitions */
#define	EC_NONE		"EC_none"
#define	EC_PRIV		"EC_priv"
#define	EC_PLATFORM	"EC_platform"	/* events private to platform */
#define	EC_DR		"EC_dr"	/* Dynamic reconfiguration event class */
#define	EC_ENV		"EC_env"	/* Environmental monitor event class */
#define	EC_DOMAIN	"EC_domain"	/* Domain event class */
#define	EC_AP_DRIVER	"EC_ap_driver"	/* Alternate Pathing event class */
#define	EC_IPMP		"EC_ipmp"	/* IP Multipathing event class */
#define	EC_DEV_ADD	"EC_dev_add"	/* device add event class */
#define	EC_DEV_REMOVE	"EC_dev_remove"	/* device remove event class */
#define	EC_DEV_BRANCH	"EC_dev_branch"	/* device tree branch event class */
#define	EC_DEV_STATUS	"EC_dev_status"	/* device status event class */
#define	EC_FM		"EC_fm"		/* FMA error report event */
#define	EC_ZFS		"EC_zfs"	/* ZFS event */
#define	EC_DATALINK	"EC_datalink"	/* datalink event */
#define	EC_VRRP		"EC_vrrp"	/* VRRP event */

/*
 * The following event class is reserved for exclusive use
 * by Sun Cluster software.
 */
#define	EC_CLUSTER	"EC_Cluster"

/*
 * The following classes are exclusively reserved for use by the
 * Solaris Volume Manager (SVM)
 */
#define	EC_SVM_CONFIG	"EC_SVM_Config"
#define	EC_SVM_STATE	"EC_SVM_State"

/*
 * EC_SVM_CONFIG subclass definitions - supporting attributes (name/value pairs)
 * are found in sys/sysevent/svm.h
 */
#define	ESC_SVM_CREATE		"ESC_SVM_Create"
#define	ESC_SVM_DELETE		"ESC_SVM_Delete"
#define	ESC_SVM_ADD		"ESC_SVM_Add"
#define	ESC_SVM_REMOVE		"ESC_SVM_Remove"
#define	ESC_SVM_REPLACE		"ESC_SVM_Replace"
#define	ESC_SVM_GROW		"ESC_SVM_Grow"
#define	ESC_SVM_RENAME_SRC	"ESC_SVM_Rename_Src"
#define	ESC_SVM_RENAME_DST	"ESC_SVM_Rename_Dst"
#define	ESC_SVM_MEDIATOR_ADD	"ESC_SVM_Mediator_Add"
#define	ESC_SVM_MEDIATOR_DELETE	"ESC_SVM_Mediator_Delete"
#define	ESC_SVM_HOST_ADD	"ESC_SVM_Host_Add"
#define	ESC_SVM_HOST_DELETE	"ESC_SVM_Host_Delete"
#define	ESC_SVM_DRIVE_ADD	"ESC_SVM_Drive_Add"
#define	ESC_SVM_DRIVE_DELETE	"ESC_SVM_Drive_Delete"
#define	ESC_SVM_DETACH		"ESC_SVM_Detach"
#define	ESC_SVM_DETACHING	"ESC_SVM_Detaching"
#define	ESC_SVM_ATTACH		"ESC_SVM_Attach"
#define	ESC_SVM_ATTACHING	"ESC_SVM_Attaching"

/*
 * EC_SVM_STATE subclass definitions - supporting attributes (name/value pairs)
 * are found in sys/sysevent/svm.h
 */
#define	ESC_SVM_INIT_START	"ESC_SVM_Init_Start"
#define	ESC_SVM_INIT_FAILED	"ESC_SVM_Init_Failed"
#define	ESC_SVM_INIT_FATAL	"ESC_SVM_Init_Fatal"
#define	ESC_SVM_INIT_SUCCESS	"ESC_SVM_Init_Success"
#define	ESC_SVM_IOERR		"ESC_SVM_Ioerr"
#define	ESC_SVM_ERRED		"ESC_SVM_Erred"
#define	ESC_SVM_LASTERRED	"ESC_SVM_Lasterred"
#define	ESC_SVM_OK		"ESC_SVM_Ok"
#define	ESC_SVM_ENABLE		"ESC_SVM_Enable"
#define	ESC_SVM_RESYNC_START	"ESC_SVM_Resync_Start"
#define	ESC_SVM_RESYNC_FAILED	"ESC_SVM_Resync_Failed"
#define	ESC_SVM_RESYNC_SUCCESS	"ESC_SVM_Resync_Success"
#define	ESC_SVM_RESYNC_DONE	"ESC_SVM_Resync_Done"
#define	ESC_SVM_HOTSPARED	"ESC_SVM_Hotspared"
#define	ESC_SVM_HS_FREED	"ESC_SVM_HS_Freed"
#define	ESC_SVM_HS_CHANGED	"ESC_SVM_HS_Changed"
#define	ESC_SVM_TAKEOVER	"ESC_SVM_Takeover"
#define	ESC_SVM_RELEASE		"ESC_SVM_Release"
#define	ESC_SVM_OPEN_FAIL	"ESC_SVM_Open_Fail"
#define	ESC_SVM_OFFLINE		"ESC_SVM_Offline"
#define	ESC_SVM_ONLINE		"ESC_SVM_Online"
#define	ESC_SVM_CHANGE		"ESC_SVM_Change"
#define	ESC_SVM_EXCHANGE	"ESC_SVM_Exchange"
#define	ESC_SVM_REGEN_START	"ESC_SVM_Regen_Start"
#define	ESC_SVM_REGEN_DONE	"ESC_SVM_Regen_Done"
#define	ESC_SVM_REGEN_FAILED	"ESC_SVM_Regen_Failed"

/*
 * EC_DR subclass definitions - supporting attributes (name/value pairs)
 * are found in sys/sysevent/dr.h
 */

/* Attachment point state change */
#define	ESC_DR_AP_STATE_CHANGE	"ESC_dr_ap_state_change"
#define	ESC_DR_REQ		"ESC_dr_req"	/* Request DR */
#define	ESC_DR_TARGET_STATE_CHANGE	"ESC_dr_target_state_change"

/*
 * EC_ENV subclass definitions - supporting attributes (name/value pairs)
 * are found in sys/sysevent/env.h
 */
#define	ESC_ENV_TEMP	"ESC_env_temp"	/* Temperature change event subclass */
#define	ESC_ENV_FAN	"ESC_env_fan"	/* Fan status change event subclass */
#define	ESC_ENV_POWER	"ESC_env_power"	/* Power supply change event subclass */
#define	ESC_ENV_LED	"ESC_env_led"	/* LED change event subclass */

/*
 * EC_DOMAIN subclass definitions - supporting attributes (name/value pairs)
 * are found in sys/sysevent/domain.h
 */

/* Domain state change */
#define	ESC_DOMAIN_STATE_CHANGE		"ESC_domain_state_change"
/* Domain loghost name change */
#define	ESC_DOMAIN_LOGHOST_CHANGE	"ESC_domain_loghost_change"

/*
 * EC_AP_DRIVER subclass definitions - supporting attributes (name/value pairs)
 * are found in sys/sysevent/ap_driver.h
 */

/* Alternate Pathing path switch */
#define	ESC_AP_DRIVER_PATHSWITCH	"ESC_ap_driver_pathswitch"
/* Alternate Pathing database commit */
#define	ESC_AP_DRIVER_COMMIT		"ESC_ap_driver_commit"
/* Alternate Pathing physical path status change */
#define	ESC_AP_DRIVER_PHYS_PATH_STATUS_CHANGE	\
	"ESC_ap_driver_phys_path_status_change"

/*
 * EC_IPMP subclass definitions - supporting attributes (name/value pairs)
 * are found in sys/sysevent/ipmp.h
 */

/* IPMP group has changed state */
#define	ESC_IPMP_GROUP_STATE		"ESC_ipmp_group_state"

/* IPMP group has been created or removed */
#define	ESC_IPMP_GROUP_CHANGE		"ESC_ipmp_group_change"

/* IPMP group has had an interface added or removed */
#define	ESC_IPMP_GROUP_MEMBER_CHANGE	"ESC_ipmp_group_member_change"

/* Interface within an IPMP group has changed state or type */
#define	ESC_IPMP_IF_CHANGE		"ESC_ipmp_if_change"

/* IPMP probe has changed state */
#define	ESC_IPMP_PROBE_STATE		"ESC_ipmp_probe_state"

/*
 * EC_DEV_ADD and EC_DEV_REMOVE subclass definitions - supporting attributes
 * (name/value pairs) are found in sys/sysevent/dev.h
 */
#define	ESC_DISK	"disk"		/* disk device */
#define	ESC_NETWORK	"network"	/* network interface */
#define	ESC_PRINTER	"printer"	/* printer device */
#define	ESC_LOFI	"lofi"		/* lofi device */

/*
 * EC_DEV_BRANCH subclass definitions - supporting attributes (name/value pairs)
 * are found in sys/sysevent/dev.h
 */

/* device tree branch added */
#define	ESC_DEV_BRANCH_ADD	"ESC_dev_branch_add"

/* device tree branch removed */
#define	ESC_DEV_BRANCH_REMOVE	"ESC_dev_branch_remove"

/*
 * EC_DEV_STATUS subclass definitions
 *
 * device capacity dynamically changed
 */
#define	ESC_DEV_DLE		"ESC_dev_dle"

/* LUN has received an eject request from the user */
#define	ESC_DEV_EJECT_REQUEST	"ESC_dev_eject_request"

/* FMA Fault and Error event protocol subclass */
#define	ESC_FM_ERROR		"ESC_FM_error"
#define	ESC_FM_ERROR_REPLAY	"ESC_FM_error_replay"

/* Service processor subclass definitions */
#define	ESC_PLATFORM_SP_RESET	"ESC_platform_sp_reset"

/*
 * EC_PWRCTL subclass definitions
 */
#define	EC_PWRCTL			"EC_pwrctl"
#define	ESC_PWRCTL_ADD			"ESC_pwrctl_add"
#define	ESC_PWRCTL_REMOVE		"ESC_pwrctl_remove"
#define	ESC_PWRCTL_WARN			"ESC_pwrctl_warn"
#define	ESC_PWRCTL_LOW			"ESC_pwrctl_low"
#define	ESC_PWRCTL_STATE_CHANGE		"ESC_pwrctl_state_change"
#define	ESC_PWRCTL_POWER_BUTTON		"ESC_pwrctl_power_button"
#define	ESC_PWRCTL_BRIGHTNESS_UP	"ESC_pwrctl_brightness_up"
#define	ESC_PWRCTL_BRIGHTNESS_DOWN	"ESC_pwrctl_brightness_down"

/* EC_ACPIEV subclass definitions */
#define	EC_ACPIEV			"EC_acpiev"
#define	ESC_ACPIEV_DISPLAY_SWITCH	"ESC_acpiev_display_switch"
#define	ESC_ACPIEV_SCREEN_LOCK		"ESC_acpiev_screen_lock"
#define	ESC_ACPIEV_SLEEP		"ESC_acpiev_sleep"
#define	ESC_ACPIEV_AUDIO_MUTE		"ESC_acpiev_audio_mute"
#define	ESC_ACPIEV_WIFI			"ESC_acpiev_wifi"
#define	ESC_ACPIEV_TOUCHPAD		"ESC_acpiev_touchpad"

/*
 * ZFS subclass definitions.  supporting attributes (name/value paris) are found
 * in sys/fs/zfs.h
 */
#define	ESC_ZFS_RESILVER_START		"ESC_ZFS_resilver_start"
#define	ESC_ZFS_RESILVER_FINISH		"ESC_ZFS_resilver_finish"
#define	ESC_ZFS_VDEV_REMOVE		"ESC_ZFS_vdev_remove"
#define	ESC_ZFS_VDEV_REMOVE_AUX		"ESC_ZFS_vdev_remove_aux"
#define	ESC_ZFS_VDEV_REMOVE_DEV		"ESC_ZFS_vdev_remove_dev"
#define	ESC_ZFS_POOL_CREATE		"ESC_ZFS_pool_create"
#define	ESC_ZFS_POOL_DESTROY		"ESC_ZFS_pool_destroy"
#define	ESC_ZFS_POOL_EXPORT		"ESC_ZFS_pool_export"
#define	ESC_ZFS_POOL_UNINITIALIZE	"ESC_ZFS_pool_uninitialize"
#define	ESC_ZFS_POOL_IMPORT		"ESC_ZFS_pool_import"
#define	ESC_ZFS_VDEV_ADD		"ESC_ZFS_vdev_add"
#define	ESC_ZFS_VDEV_ATTACH		"ESC_ZFS_vdev_attach"
#define	ESC_ZFS_VDEV_CLEAR		"ESC_ZFS_vdev_clear"
#define	ESC_ZFS_VDEV_CHECK		"ESC_ZFS_vdev_check"
#define	ESC_ZFS_VDEV_ONLINE		"ESC_ZFS_vdev_online"
#define	ESC_ZFS_CONFIG_SYNC		"ESC_ZFS_config_sync"
#define	ESC_ZFS_SCRUB_START		"ESC_ZFS_scrub_start"
#define	ESC_ZFS_SCRUB_FINISH		"ESC_ZFS_scrub_finish"
#define	ESC_ZFS_VDEV_SPARE		"ESC_ZFS_vdev_spare"
#define	ESC_ZFS_BOOTFS_VDEV_ATTACH	"ESC_ZFS_bootfs_vdev_attach"
#define	ESC_ZFS_POOL_REGUID		"ESC_ZFS_pool_reguid"
#define	ESC_ZFS_POOL_HISTORY		"ESC_ZFS_pool_history"
#define	ESC_ZFS_VDEV_ADD		"ESC_ZFS_vdev_add"

/*
 * datalink subclass definitions.
 */
#define	ESC_DATALINK_PHYS_ADD	"ESC_datalink_phys_add"	/* new physical link */

/*
 * VRRP subclass definitions. Supporting attributes (name/value paris) are
 * found in sys/sysevent/vrrp.h
 */
#define	ESC_VRRP_STATE_CHANGE	"ESC_vrrp_state_change"

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_SYSEVENT_EVENTDEFS_H */

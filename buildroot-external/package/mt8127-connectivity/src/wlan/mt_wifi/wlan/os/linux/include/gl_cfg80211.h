/*
** $Id: //Department/DaVinci/BRANCHES/MT6620_WIFI_DRIVER_V2_3/os/linux/include/gl_cfg80211.h#1 $
*/

/*! \file   gl_cfg80211.h
    \brief  This file is for Portable Driver linux cfg80211 support.
*/



/*
** $Log: gl_cfg80211.h $
** 
** 09 03 2013 cp.wu
** add path for reassociation
** 
** 09 12 2012 wcpadmin
** [ALPS00276400] Remove MTK copyright and legal header on GPL/LGPL related packages
** .
** 
** 08 30 2012 chinglan.wang
** [ALPS00349664] [6577JB][WIFI] Phone can not connect to AP secured with AES via WPS in 802.11n Only
** .
 *
*/

#ifndef _GL_CFG80211_H
#define _GL_CFG80211_H

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>

#include "gl_os.h"

/* cfg80211 hooks; invoked with the wiphy mutex held */
int
mtk_cfg80211_change_iface (
    struct wiphy *wiphy,
    struct net_device *ndev,
    enum nl80211_iftype type,
    struct vif_params *params
    );

int
mtk_cfg80211_add_key (
    struct wiphy *wiphy,
    struct net_device *ndev,
    int link_id,
    u8 key_index,
    bool pairwise,
    const u8 *mac_addr,
    struct key_params *params
    );

int
mtk_cfg80211_get_key (
    struct wiphy *wiphy,
    struct net_device *ndev,
    int link_id,
    u8 key_index,
    bool pairwise,
    const u8 *mac_addr,
    void *cookie,
    void (*callback)(void *cookie, struct key_params*)
    );

int
mtk_cfg80211_del_key (
    struct wiphy *wiphy,
    struct net_device *ndev,
    int link_id,
    u8 key_index,
    bool pairwise,
    const u8 *mac_addr
    );

int
mtk_cfg80211_set_default_key (
    struct wiphy *wiphy,
    struct net_device *ndev,
    int link_id,
    u8 key_index,
    bool unicast,
    bool multicast
    );

int
mtk_cfg80211_set_default_mgmt_key(
    struct wiphy *wiphy,
    struct net_device *netdev,
    int link_id,
    u8 key_index
    );

int
mtk_cfg80211_get_station (
    struct wiphy *wiphy,
    struct net_device *ndev,
    const u8 *mac,
    struct station_info *sinfo
    );

int
mtk_cfg80211_scan (
    struct wiphy *wiphy,
    struct cfg80211_scan_request *request
    );

int
mtk_cfg80211_connect (
    struct wiphy *wiphy,
    struct net_device *ndev,
    struct cfg80211_connect_params *sme
    );

int
mtk_cfg80211_disconnect (
    struct wiphy *wiphy,
    struct net_device *ndev,
    u16 reason_code
    );

int
mtk_cfg80211_join_ibss (
    struct wiphy *wiphy,
    struct net_device *ndev,
    struct cfg80211_ibss_params *params
    );

int
mtk_cfg80211_leave_ibss (
    struct wiphy *wiphy,
    struct net_device *ndev
    );

int
mtk_cfg80211_set_power_mgmt (
    struct wiphy *wiphy,
    struct net_device *ndev,
    bool enabled,
    int timeout
    );

int
mtk_cfg80211_set_pmksa (
    struct wiphy *wiphy,
    struct net_device *ndev,
    struct cfg80211_pmksa *pmksa
    );

int
mtk_cfg80211_del_pmksa (
    struct wiphy *wiphy,
    struct net_device *ndev,
    struct cfg80211_pmksa *pmksa
    );

int
mtk_cfg80211_flush_pmksa (
    struct wiphy *wiphy,
    struct net_device *ndev
    );

#endif /* _GL_CFG80211_H */

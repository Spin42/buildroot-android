/*
** $Id: @(#) gl_cfg80211.c@@
*/

/*! \file   gl_cfg80211.c
    \brief  Main routines for supporintg MT6620 cfg80211 control interface

    This file contains the support routines of Linux driver for MediaTek Inc. 802.11
    Wireless LAN Adapters.
*/



/*
** $Log: gl_cfg80211.c $
** 
** 09 05 2013 cp.wu
** correct length to pass to wlanoidSetBssid()
**
** 09 04 2013 cp.wu
** fix typo
**
** 09 03 2013 cp.wu
** add path for reassociation
**
** 11 23 2012 yuche.tsai
** [ALPS00398671] [Acer-Tablet] Remove Wi-Fi Direct completely
** Fix bug of WiFi may reboot under user load, when WiFi Direct is removed..
** 
** 09 12 2012 wcpadmin
** [ALPS00276400] Remove MTK copyright and legal header on GPL/LGPL related packages
** .
** 
** 08 30 2012 chinglan.wang
** [ALPS00349664] [6577JB][WIFI] Phone can not connect to AP secured with AES via WPS in 802.11n Only
** .
 *
**
*/

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/
#include "gl_os.h"
#include "debug.h"
#include "wlan_lib.h"
#include "gl_wext.h"
#include "precomp.h"
#include <net/netlink.h>
#include <net/cfg80211.h>
#include "gl_cfg80211.h"

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/

/* workaround for some ANR CRs. if suppliant is blocked longer than 10s, wifi hal will tell wifiMonitor
to teminate. for the case which can block supplicant 10s is to del key more than 5 times. the root cause
is that there is no resource in TC4, so del key command was not able to set, and then oid
timeout was happed. if we found the root cause why fw couldn't release TC resouce, we will remove this
workaround */
static UINT_8 gucKeyIndex = 255;

/*******************************************************************************
*                           P R I V A T E   D A T A
********************************************************************************
*/

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/

/*******************************************************************************
*                   F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/
extern void wlanHandleSystemResume(void);
extern void wlanHandleSystemSuspend(void);

/*******************************************************************************
*                              F U N C T I O N S
********************************************************************************
*/

/*----------------------------------------------------------------------------*/
/*!
 * @brief Find a WPA/RSN information element by element ID. For the vendor
 *        specific ID 0xDD only the WPA OUI (00:50:f2:01) matches.
 *
 * @retval TRUE   found, *ppucDesiredIE points at the element
 *         FALSE  not found
 */
/*----------------------------------------------------------------------------*/
BOOLEAN
wextSrchDesiredWPAIE (
    IN  PUINT_8         pucIEStart,
    IN  INT_32          i4TotalIeLen,
    IN  UINT_8          ucDesiredElemId,
    OUT PUINT_8         *ppucDesiredIE
    )
{
    INT_32 i4InfoElemLen;

    ASSERT(pucIEStart);
    ASSERT(ppucDesiredIE);

    while (i4TotalIeLen >= 2) {
        i4InfoElemLen = (INT_32) pucIEStart[1] + 2;

        if (pucIEStart[0] == ucDesiredElemId && i4InfoElemLen <= i4TotalIeLen) {
            if (ucDesiredElemId != 0xDD) {
                *ppucDesiredIE = &pucIEStart[0];
                return TRUE;
            }
            else if (pucIEStart[1] >= 4 &&
                     memcmp(&pucIEStart[2], "\x00\x50\xf2\x01", 4) == 0) {
                *ppucDesiredIE = &pucIEStart[0];
                return TRUE;
            }
        }

        i4TotalIeLen -= i4InfoElemLen;
        pucIEStart += i4InfoElemLen;
    }

    return FALSE;
}

#if CFG_SUPPORT_WPS
/*----------------------------------------------------------------------------*/
/*!
 * @brief Find a WPS information element by element ID. For the vendor
 *        specific ID 0xDD only the WPS OUI (00:50:f2:04) matches.
 */
/*----------------------------------------------------------------------------*/
BOOLEAN
wextSrchDesiredWPSIE (
    IN PUINT_8 pucIEStart,
    IN INT_32 i4TotalIeLen,
    IN UINT_8 ucDesiredElemId,
    OUT PUINT_8 *ppucDesiredIE
    )
{
    INT_32 i4InfoElemLen;

    ASSERT(pucIEStart);
    ASSERT(ppucDesiredIE);

    while (i4TotalIeLen >= 2) {
        i4InfoElemLen = (INT_32) pucIEStart[1] + 2;

        if (pucIEStart[0] == ucDesiredElemId && i4InfoElemLen <= i4TotalIeLen) {
            if (ucDesiredElemId != 0xDD) {
                *ppucDesiredIE = &pucIEStart[0];
                return TRUE;
            }
            else if (pucIEStart[1] >= 4 &&
                     memcmp(&pucIEStart[2], "\x00\x50\xf2\x04", 4) == 0) {
                *ppucDesiredIE = &pucIEStart[0];
                return TRUE;
            }
        }

        i4TotalIeLen -= i4InfoElemLen;
        pucIEStart += i4InfoElemLen;
    }

    return FALSE;
}
#endif


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for change STA type between
 *        1. Infrastructure Client (Non-AP STA)
 *        2. Ad-Hoc IBSS
 *
 * @param
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int 
mtk_cfg80211_change_iface (
    struct wiphy *wiphy,
    struct net_device *ndev,
    enum nl80211_iftype type,
    struct vif_params *params
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;
    ENUM_PARAM_OP_MODE_T eOpMode;
    UINT_32 u4BufLen;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

    if(type == NL80211_IFTYPE_STATION) {
        eOpMode = NET_TYPE_INFRA;
    }
    else if(type == NL80211_IFTYPE_ADHOC) {
        eOpMode = NET_TYPE_IBSS;
    }
    else {
        return -EINVAL;
    }

    rStatus = kalIoctl(prGlueInfo,
            wlanoidSetInfrastructureMode,
            &eOpMode,
            sizeof(eOpMode),
            FALSE,
            FALSE,
            TRUE,
            FALSE,
            &u4BufLen);
    
    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(REQ, WARN, ("set infrastructure mode error:%x\n", rStatus));
    }

    /* reset wpa info */
    prGlueInfo->rWpaInfo.u4WpaVersion = IW_AUTH_WPA_VERSION_DISABLED;
    prGlueInfo->rWpaInfo.u4KeyMgmt = 0;
    prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_NONE;
    prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_NONE;
    prGlueInfo->rWpaInfo.u4AuthAlg = IW_AUTH_ALG_OPEN_SYSTEM;
#if CFG_SUPPORT_802_11W
    prGlueInfo->rWpaInfo.u4Mfp = IW_AUTH_MFP_DISABLED;
#endif

    return 0;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for adding key 
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_add_key (
    struct wiphy *wiphy,
    struct net_device *ndev,
    int link_id,
    u8 key_index,
    bool pairwise,
    const u8 *mac_addr,
    struct key_params *params
    )
{
    PARAM_KEY_T rKey;
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;
    INT_32 i4Rslt = -EINVAL;
    UINT_32 u4BufLen = 0;
    UINT_8 tmp1[8];
    UINT_8 tmp2[8];

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);
    
    kalMemZero(&rKey, sizeof(PARAM_KEY_T));

    rKey.u4KeyIndex = key_index;
    
    if(mac_addr) {
        COPY_MAC_ADDR(rKey.arBSSID, mac_addr);
        if ((rKey.arBSSID[0] == 0x00) && (rKey.arBSSID[1] == 0x00) && (rKey.arBSSID[2] == 0x00) &&
            (rKey.arBSSID[3] == 0x00) && (rKey.arBSSID[4] == 0x00) && (rKey.arBSSID[5] == 0x00)) {
            rKey.arBSSID[0] = 0xff;
            rKey.arBSSID[1] = 0xff;
            rKey.arBSSID[2] = 0xff;
            rKey.arBSSID[3] = 0xff;
            rKey.arBSSID[4] = 0xff;
            rKey.arBSSID[5] = 0xff;
        }
        if (rKey.arBSSID[0] != 0xFF) {
            rKey.u4KeyIndex |= BIT(31);
            if ((rKey.arBSSID[0] != 0x00) || (rKey.arBSSID[1] != 0x00) || (rKey.arBSSID[2] != 0x00) ||
                (rKey.arBSSID[3] != 0x00) || (rKey.arBSSID[4] != 0x00) || (rKey.arBSSID[5] != 0x00))
            rKey.u4KeyIndex |= BIT(30);
        }
    }
    else {
            rKey.arBSSID[0] = 0xff;
            rKey.arBSSID[1] = 0xff;
            rKey.arBSSID[2] = 0xff;
            rKey.arBSSID[3] = 0xff;
            rKey.arBSSID[4] = 0xff;
            rKey.arBSSID[5] = 0xff;
            //rKey.u4KeyIndex |= BIT(31); //Enable BIT 31 will make tx use bc key id, should use pairwise key id 0 
    }
    
    if(params->key) {
        //rKey.aucKeyMaterial[0] = kalMemAlloc(params->key_len, VIR_MEM_TYPE);
        kalMemCopy(rKey.aucKeyMaterial, params->key, params->key_len);
        if (params->key_len == 32) {
            kalMemCopy(tmp1, &params->key[16], 8);
            kalMemCopy(tmp2, &params->key[24], 8);        
            kalMemCopy(&rKey.aucKeyMaterial[16], tmp2, 8);
            kalMemCopy(&rKey.aucKeyMaterial[24], tmp1, 8);
        }
    }

    rKey.u4KeyLength = params->key_len;
    rKey.u4Length = offsetof(PARAM_KEY_T, aucKeyMaterial) + rKey.u4KeyLength;
    
    rStatus = kalIoctl(prGlueInfo,
            wlanoidSetAddKey,
            &rKey,
            rKey.u4Length,
            FALSE,
            FALSE,
            TRUE,
            FALSE,
            &u4BufLen);
    
    if (rStatus == WLAN_STATUS_SUCCESS)
        i4Rslt = 0;

    return i4Rslt;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for getting key for specified STA
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
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
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

#if 1
    printk("--> %s()\n", __func__);
#endif

    /* not implemented */

    return -EINVAL;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for removing key for specified STA
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_del_key (
    struct wiphy *wiphy,
    struct net_device *ndev,
    int link_id,
    u8 key_index,
    bool pairwise,
    const u8 *mac_addr
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS rStatus = WLAN_STATUS_SUCCESS;
    PARAM_REMOVE_KEY_T rRemoveKey;
    UINT_32 u4BufLen = 0;
    INT_32 i4Rslt = -EINVAL;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

    kalMemZero(&rRemoveKey, sizeof(PARAM_REMOVE_KEY_T));
    if(mac_addr)
        COPY_MAC_ADDR(rRemoveKey.arBSSID, mac_addr);
	else if (key_index > gucKeyIndex) {/* bypass the next remove key operation */
		gucKeyIndex = key_index;
		return -EBUSY;
	}
	else /* new operation, reset gucKeyIndex */
		gucKeyIndex = 255;
    rRemoveKey.u4KeyIndex = key_index;
    rRemoveKey.u4Length = sizeof(PARAM_REMOVE_KEY_T);
    

    rStatus = kalIoctl(prGlueInfo,
            wlanoidSetRemoveKey,
            &rRemoveKey,
            rRemoveKey.u4Length,
            FALSE,
            FALSE,
            TRUE,
            FALSE,
            &u4BufLen);

    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(REQ, WARN, ("remove key error:%x\n", rStatus));
		if (WLAN_STATUS_FAILURE == rStatus && mac_addr) {
			i4Rslt = -EBUSY;
			gucKeyIndex = key_index;
		}
    }
    else {
		gucKeyIndex = 255;
        i4Rslt = 0;
    }

    return i4Rslt;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for setting default key on an interface
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int 
mtk_cfg80211_set_default_key (
    struct wiphy *wiphy,
    struct net_device *ndev,
    int link_id,
    u8 key_index,
    bool unicast,
    bool multicast
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

#if 1
    printk("--> %s()\n", __func__);
#endif

    /* not implemented */

    return WLAN_STATUS_SUCCESS;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for setting set_default_mgmt_ke on an interface
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int 
mtk_cfg80211_set_default_mgmt_key(
	struct wiphy *wiphy,
	struct net_device *netdev,
	int link_id,
	u8 key_index
	)
{
	return 0;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for getting station information such as RSSI
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/

int
mtk_cfg80211_get_station (
    struct wiphy *wiphy,
    struct net_device *ndev,
    const u8 *mac,
    struct station_info *sinfo
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS rStatus;
    PARAM_MAC_ADDRESS arBssid;
    UINT_32 u4BufLen, u4Rate;
    UINT_32 u8diffTxBad,u8diffRetry;
    INT_32 i4Rssi;
    PARAM_802_11_STATISTICS_STRUCT_T rStatistics;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

    kalMemZero(arBssid, MAC_ADDR_LEN);
    wlanQueryInformation(prGlueInfo->prAdapter,
            wlanoidQueryBssid,
            &arBssid[0],
            sizeof(arBssid),
            &u4BufLen);

    /* 1. check BSSID */
    if(UNEQUAL_MAC_ADDR(arBssid, mac)) {
        /* wrong MAC address */
        DBGLOG(REQ, WARN, ("incorrect BSSID: ["MACSTR"] currently connected BSSID["MACSTR"]\n", 
                    MAC2STR(mac), MAC2STR(arBssid)));
        return -ENOENT;
    }

    /* 2. fill TX rate */
    rStatus = kalIoctl(prGlueInfo,
        wlanoidQueryLinkSpeed,
        &u4Rate,
        sizeof(u4Rate),
        TRUE,
        FALSE,
        FALSE,
        FALSE,
        &u4BufLen);

    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(REQ, WARN, ("unable to retrieve link speed\n"));
    }
    else {
        sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
        sinfo->txrate.legacy = u4Rate / 1000; /* convert from 100bps to 100kbps */
    }

    if(prGlueInfo->eParamMediaStateIndicated != PARAM_MEDIA_STATE_CONNECTED) {
        /* not connected */
        DBGLOG(REQ, WARN, ("not yet connected\n"));
    }
    else {
        /* 3. fill RSSI */
        rStatus = kalIoctl(prGlueInfo,
                wlanoidQueryRssi,
                &i4Rssi,
                sizeof(i4Rssi),
                TRUE,
                FALSE,
                FALSE,
                FALSE,
                &u4BufLen);
        
        if (rStatus != WLAN_STATUS_SUCCESS) {
            DBGLOG(REQ, WARN, ("unable to retrieve link speed\n"));
        }
        else {
            sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
	    //in the cfg80211 layer, the signal is a signed char variable.
            if(i4Rssi < -128)
		sinfo->signal = -128;
	    else
            sinfo->signal = i4Rssi; /* dBm */
        }
    sinfo->rx_packets = prGlueInfo->rNetDevStats.rx_packets;
        
        /* 4. Fill Tx OK and Tx Bad */
        
    sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_PACKETS);
    sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_FAILED);
   {
            WLAN_STATUS rStatus;
            kalMemZero(&rStatistics, sizeof(rStatistics));
            /* Get Tx OK/Fail cnt from AIS statistic counter */    
            rStatus = kalIoctl(prGlueInfo,
                    wlanoidQueryStatisticsPL,
                    &rStatistics,
                    sizeof(rStatistics),
                    TRUE,
                    TRUE,
                    TRUE,
                    FALSE,
                    &u4BufLen);

            if (rStatus != WLAN_STATUS_SUCCESS) {
                DBGLOG(REQ, WARN, ("unable to retrieive statistic\n"));
		printk("unable to retrieve statics");
            }
            else {
		INT_32 i4RssiThreshold = -85; /* set rssi threshold -85dBm */
		UINT_32 u4LinkspeedThreshold = 55; /* set link speed threshold 5.5Mbps */
		BOOLEAN fgWeighted = 0;

		/* calculate difference */
		u8diffTxBad =  rStatistics.rFailedCount.QuadPart - prGlueInfo->u8Statistic[0];
		u8diffRetry =  rStatistics.rRetryCount.QuadPart - prGlueInfo->u8Statistic[1];
		/* restore counters */
		prGlueInfo->u8Statistic[0] = rStatistics.rFailedCount.QuadPart;
		prGlueInfo->u8Statistic[1] = rStatistics.rRetryCount.QuadPart;

		/* check threshold is valid */
		if(prGlueInfo->fgPoorlinkValid){
		  if(prGlueInfo->i4RssiThreshold)
	  		i4RssiThreshold = prGlueInfo->i4RssiThreshold;
		  if(prGlueInfo->u4LinkspeedThreshold)
			u4LinkspeedThreshold = prGlueInfo->u4LinkspeedThreshold;
   }
		/* add weighted to fail counter */	
                if(sinfo->txrate.legacy < u4LinkspeedThreshold || sinfo->signal < i4RssiThreshold ){
	    		prGlueInfo->u8TotalFailCnt += (u8diffTxBad*16 + u8diffRetry);
			fgWeighted = 1;
		}else{
	    		prGlueInfo->u8TotalFailCnt += u8diffTxBad;
		}
		/* report counters */
                prGlueInfo->rNetDevStats.tx_packets = rStatistics.rTransmittedFragmentCount.QuadPart ; 
                prGlueInfo->rNetDevStats.tx_errors = prGlueInfo->u8TotalFailCnt;

                sinfo->tx_packets = prGlueInfo->rNetDevStats.tx_packets;
    sinfo->tx_failed = prGlueInfo->rNetDevStats.tx_errors;

		printk("poorlink get state G(%d)F(%d)dbad(%d)dretry(%d)LT(%d)RT(%d)W(%d)(%d)\n",
		sinfo->tx_packets,
		sinfo->tx_failed,
        (int)u8diffTxBad,
		(int)u8diffRetry,
		sinfo->txrate.legacy,
		sinfo->signal,
		(int)fgWeighted,
		(int)rStatistics.rMultipleRetryCount.QuadPart);
            }                
}

    }
    return 0;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to do a scan
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
static PARAM_SCAN_REQUEST_EXT_T rScanRequest;
int 
mtk_cfg80211_scan (
    struct wiphy *wiphy,
    struct cfg80211_scan_request *request
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS rStatus;
    UINT_32 u4BufLen;
//    PARAM_SCAN_REQUEST_EXT_T rScanRequest;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

	DBGLOG(REQ, INFO, ("mtk_cfg80211_scan\n"));
    kalMemZero(&rScanRequest, sizeof(PARAM_SCAN_REQUEST_EXT_T));
    
    /* check if there is any pending scan not yet finished */
    if(prGlueInfo->prScanRequest != NULL) {
		DBGLOG(REQ, INFO, ("prGlueInfo->prScanRequest != NULL\n"));
        return -EBUSY;
    }

    if(request->n_ssids == 0) {
        rScanRequest.rSsid.u4SsidLen = 0;
    }
    else if(request->n_ssids == 1) {
        COPY_SSID(rScanRequest.rSsid.aucSsid, rScanRequest.rSsid.u4SsidLen, request->ssids[0].ssid, request->ssids[0].ssid_len);
    }
    else {
		DBGLOG(REQ, INFO, ("request->n_ssids:%d\n", request->n_ssids));
        return -EINVAL;
    }

    if(request->ie_len > 0) {
        rScanRequest.u4IELength = request->ie_len;
        rScanRequest.pucIE = (PUINT_8)(request->ie);
    }
    else {
        rScanRequest.u4IELength = 0;
    }

    prGlueInfo->prScanRequest = request;

    rStatus = kalIoctl(prGlueInfo,
        wlanoidSetBssidListScanExt,
        &rScanRequest,
        sizeof(PARAM_SCAN_REQUEST_EXT_T),
        FALSE,
        FALSE,
        FALSE,
        FALSE,
        &u4BufLen);

    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(REQ, INFO, ("scan error:%x\n", rStatus));
		prGlueInfo->prScanRequest = NULL;
        return -EINVAL;
    }

    return 0;
}

static UINT_8 wepBuf[48];

/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to connect to 
 *        the ESS with the specified parameters
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_connect (
    struct wiphy *wiphy,
    struct net_device *ndev,
    struct cfg80211_connect_params *sme
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS rStatus;
    UINT_32 u4BufLen;
    ENUM_PARAM_ENCRYPTION_STATUS_T eEncStatus;
    ENUM_PARAM_AUTH_MODE_T eAuthMode;
    UINT_32 cipher;
    PARAM_SSID_T rNewSsid;
    BOOLEAN fgCarryWPSIE = FALSE;
    ENUM_PARAM_OP_MODE_T eOpMode;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

	DBGLOG(REQ, INFO, ("[wlan] mtk_cfg80211_connect %p %zu\n",
		   sme->ie, sme->ie_len));

    if (prGlueInfo->prAdapter->rWifiVar.rConnSettings.eOPMode > NET_TYPE_AUTO_SWITCH)
		eOpMode = NET_TYPE_AUTO_SWITCH;
	else 
	    eOpMode = prGlueInfo->prAdapter->rWifiVar.rConnSettings.eOPMode;
	
	rStatus = kalIoctl(prGlueInfo,
		wlanoidSetInfrastructureMode,
		&eOpMode,
		sizeof(eOpMode),
		FALSE,
		FALSE,
		TRUE,
		FALSE,
		&u4BufLen);

	if (rStatus != WLAN_STATUS_SUCCESS) {
		DBGLOG(INIT, INFO, ("wlanoidSetInfrastructureMode fail 0x%x\n", rStatus));
		return -EFAULT;
	}

	/* after set operation mode, key table are cleared */

	/* reset wpa info */
	prGlueInfo->rWpaInfo.u4WpaVersion = IW_AUTH_WPA_VERSION_DISABLED;
	prGlueInfo->rWpaInfo.u4KeyMgmt = 0;
	prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_NONE;
	prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_NONE;
	prGlueInfo->rWpaInfo.u4AuthAlg = IW_AUTH_ALG_OPEN_SYSTEM;
#if CFG_SUPPORT_802_11W
	prGlueInfo->rWpaInfo.u4Mfp = IW_AUTH_MFP_DISABLED;
#endif

    if (sme->crypto.wpa_versions & NL80211_WPA_VERSION_1)
        prGlueInfo->rWpaInfo.u4WpaVersion = IW_AUTH_WPA_VERSION_WPA;
    else if (sme->crypto.wpa_versions & NL80211_WPA_VERSION_2)
        prGlueInfo->rWpaInfo.u4WpaVersion = IW_AUTH_WPA_VERSION_WPA2;
    else
        prGlueInfo->rWpaInfo.u4WpaVersion = IW_AUTH_WPA_VERSION_DISABLED;
 
    switch (sme->auth_type) {
    case NL80211_AUTHTYPE_OPEN_SYSTEM:
        prGlueInfo->rWpaInfo.u4AuthAlg = IW_AUTH_ALG_OPEN_SYSTEM;
        break;
    case NL80211_AUTHTYPE_SHARED_KEY:
        prGlueInfo->rWpaInfo.u4AuthAlg = IW_AUTH_ALG_SHARED_KEY;
        break;
    default:
        prGlueInfo->rWpaInfo.u4AuthAlg = IW_AUTH_ALG_OPEN_SYSTEM | IW_AUTH_ALG_SHARED_KEY;
        break;
    }

    if (sme->crypto.n_ciphers_pairwise) {
        prGlueInfo->prAdapter->rWifiVar.rConnSettings.rRsnInfo.au4PairwiseKeyCipherSuite[0] = sme->crypto.ciphers_pairwise[0];
        switch (sme->crypto.ciphers_pairwise[0]) {
        case WLAN_CIPHER_SUITE_WEP40:
            prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_WEP40;
            break;
        case WLAN_CIPHER_SUITE_WEP104:
            prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_WEP104;
            break;
        case WLAN_CIPHER_SUITE_TKIP:
            prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_TKIP;
            break;
        case WLAN_CIPHER_SUITE_CCMP:
            prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_CCMP;
            break;
        case WLAN_CIPHER_SUITE_AES_CMAC:
            prGlueInfo->rWpaInfo.u4CipherPairwise = IW_AUTH_CIPHER_CCMP;
            break;
        default:
            DBGLOG(REQ, WARN, ("invalid cipher pairwise (%d)\n",
                   sme->crypto.ciphers_pairwise[0]));
            return -EINVAL;
        }
    }

    if (sme->crypto.cipher_group) {
        prGlueInfo->prAdapter->rWifiVar.rConnSettings.rRsnInfo.u4GroupKeyCipherSuite = sme->crypto.cipher_group;
        switch (sme->crypto.cipher_group) {
        case WLAN_CIPHER_SUITE_WEP40:
            prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_WEP40;
            break;          
        case WLAN_CIPHER_SUITE_WEP104:
            prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_WEP104;
            break;
        case WLAN_CIPHER_SUITE_TKIP:
            prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_TKIP;
            break;
        case WLAN_CIPHER_SUITE_CCMP:
            prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_CCMP;
            break;
        case WLAN_CIPHER_SUITE_AES_CMAC:
            prGlueInfo->rWpaInfo.u4CipherGroup = IW_AUTH_CIPHER_CCMP;
            break;
        default:
            DBGLOG(REQ, WARN, ("invalid cipher group (%d)\n",
                   sme->crypto.cipher_group));
            return -EINVAL;
        }
    }

    if (sme->crypto.n_akm_suites) {
        prGlueInfo->prAdapter->rWifiVar.rConnSettings.rRsnInfo.au4AuthKeyMgtSuite[0] = sme->crypto.akm_suites[0];
        if (prGlueInfo->rWpaInfo.u4WpaVersion == IW_AUTH_WPA_VERSION_WPA) {
            switch (sme->crypto.akm_suites[0]) {
            case WLAN_AKM_SUITE_8021X:
                eAuthMode = AUTH_MODE_WPA;
                break;
            case WLAN_AKM_SUITE_PSK:
                eAuthMode = AUTH_MODE_WPA_PSK;
            break;
            default:
                DBGLOG(REQ, WARN, ("invalid cipher group (%d)\n",
                       sme->crypto.cipher_group));
                return -EINVAL;
            }
        } else if (prGlueInfo->rWpaInfo.u4WpaVersion == IW_AUTH_WPA_VERSION_WPA2) {
            switch (sme->crypto.akm_suites[0]) {
            case WLAN_AKM_SUITE_8021X:
            eAuthMode = AUTH_MODE_WPA2;
            break;
            case WLAN_AKM_SUITE_PSK:
            eAuthMode = AUTH_MODE_WPA2_PSK;
            break;
            default:
                DBGLOG(REQ, WARN, ("invalid cipher group (%d)\n",
                       sme->crypto.cipher_group));
                return -EINVAL;
            }
        }
    }

    if (prGlueInfo->rWpaInfo.u4WpaVersion == IW_AUTH_WPA_VERSION_DISABLED) {
        eAuthMode = (prGlueInfo->rWpaInfo.u4AuthAlg == IW_AUTH_ALG_OPEN_SYSTEM) ?
             AUTH_MODE_OPEN : AUTH_MODE_AUTO_SWITCH;
    }

    prGlueInfo->rWpaInfo.fgPrivacyInvoke = sme->privacy;

    prGlueInfo->fgWpsActive = FALSE;

    if (sme->ie && sme->ie_len > 0) {
        WLAN_STATUS rStatus;
        UINT_32 u4BufLen;
        PUINT_8 prDesiredIE = NULL;

#if CFG_SUPPORT_WPS2
        if (wextSrchDesiredWPSIE((PUINT_8)sme->ie,
                    sme->ie_len,
                    0xDD,
                    (PUINT_8 *)&prDesiredIE)) {
            prGlueInfo->fgWpsActive = TRUE;
            fgCarryWPSIE = TRUE;

            rStatus = kalIoctl(prGlueInfo,
                    wlanoidSetWSCAssocInfo,
                    prDesiredIE,
                    IE_SIZE(prDesiredIE),
                    FALSE,
                    FALSE,
                    FALSE,
                    FALSE,
                    &u4BufLen);
            if (rStatus != WLAN_STATUS_SUCCESS) {
                DBGLOG(SEC, WARN, ("WSC] set WSC assoc info error:%x\n", rStatus));
            }
        }
#endif

    }

    /* clear WSC Assoc IE buffer in case WPS IE is not detected */
    if(fgCarryWPSIE == FALSE) {
        kalMemZero(&prGlueInfo->aucWSCAssocInfoIE, 200);
        prGlueInfo->u2WSCAssocInfoIELen = 0;
    }

    rStatus = kalIoctl(prGlueInfo,
            wlanoidSetAuthMode,
            &eAuthMode,
            sizeof(eAuthMode),
            FALSE,
            FALSE,
            FALSE,
            FALSE,
            &u4BufLen);
    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(REQ, WARN, ("set auth mode error:%x\n", rStatus));
    }

    cipher = prGlueInfo->rWpaInfo.u4CipherGroup | prGlueInfo->rWpaInfo.u4CipherPairwise;

    if (prGlueInfo->rWpaInfo.fgPrivacyInvoke) {
        if (cipher & IW_AUTH_CIPHER_CCMP) {
            eEncStatus = ENUM_ENCRYPTION3_ENABLED;
        }
        else if (cipher & IW_AUTH_CIPHER_TKIP) {
            eEncStatus = ENUM_ENCRYPTION2_ENABLED;
        }
        else if (cipher & (IW_AUTH_CIPHER_WEP104 | IW_AUTH_CIPHER_WEP40)) {
            eEncStatus = ENUM_ENCRYPTION1_ENABLED;
        }
        else if (cipher & IW_AUTH_CIPHER_NONE){
            if (prGlueInfo->rWpaInfo.fgPrivacyInvoke)
                eEncStatus = ENUM_ENCRYPTION1_ENABLED;
            else
                eEncStatus = ENUM_ENCRYPTION_DISABLED;
        }
        else {
            eEncStatus = ENUM_ENCRYPTION_DISABLED;
        }
    }
    else {
        eEncStatus = ENUM_ENCRYPTION_DISABLED;
    }
    
    rStatus = kalIoctl(prGlueInfo,
            wlanoidSetEncryptionStatus,
            &eEncStatus,
            sizeof(eEncStatus),
            FALSE,
            FALSE,
            FALSE,
            FALSE,
            &u4BufLen);
    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(REQ, WARN, ("set encryption mode error:%x\n", rStatus));
    }

    if (sme->key_len != 0 && prGlueInfo->rWpaInfo.u4WpaVersion == IW_AUTH_WPA_VERSION_DISABLED) {
        P_PARAM_WEP_T prWepKey = (P_PARAM_WEP_T) wepBuf;
        
        prWepKey->u4Length = 12 + sme->key_len;
        prWepKey->u4KeyLength = (UINT_32) sme->key_len;
        prWepKey->u4KeyIndex = (UINT_32) sme->key_idx;
        prWepKey->u4KeyIndex |= BIT(31);
        if (prWepKey->u4KeyLength > 32) {
            DBGLOG(REQ, WARN, ("Too long key length (%u)\n", prWepKey->u4KeyLength));
            return -EINVAL;
        }
        kalMemCopy(prWepKey->aucKeyMaterial, sme->key, prWepKey->u4KeyLength);

        rStatus = kalIoctl(prGlueInfo,
                     wlanoidSetAddWep,
                     prWepKey,
                     prWepKey->u4Length,
                     FALSE,
                     FALSE,
                     TRUE,
                     FALSE,
                     &u4BufLen);
        
        if (rStatus != WLAN_STATUS_SUCCESS) {
            DBGLOG(INIT, INFO, ("wlanoidSetAddWep fail 0x%x\n", rStatus));
            return -EFAULT;
        }
    }

    if(sme->ssid_len > 0) {
        /* connect by SSID */
        COPY_SSID(rNewSsid.aucSsid, rNewSsid.u4SsidLen, sme->ssid, sme->ssid_len);

        rStatus = kalIoctl(prGlueInfo,
                wlanoidSetSsid,
                (PVOID) &rNewSsid,
                sizeof(PARAM_SSID_T),
                FALSE,
                FALSE,
                TRUE,
                FALSE,
                &u4BufLen);

        if (rStatus != WLAN_STATUS_SUCCESS) {
            DBGLOG(REQ, WARN, ("set SSID:%x\n", rStatus));
            return -EINVAL;
        }
    }
    else {
        /* connect by BSSID */
        rStatus = kalIoctl(prGlueInfo,
                wlanoidSetBssid,
                (PVOID) sme->bssid,
                MAC_ADDR_LEN,
                FALSE,
                FALSE,
                TRUE,
                FALSE,
                &u4BufLen);

        if (rStatus != WLAN_STATUS_SUCCESS) {
            DBGLOG(REQ, WARN, ("set BSSID:%x\n", rStatus));
            return -EINVAL;
        }
    }

    return 0;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to disconnect from
 *        currently connected ESS
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int 
mtk_cfg80211_disconnect (
    struct wiphy *wiphy,
    struct net_device *ndev,
    u16 reason_code
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS rStatus;
    UINT_32 u4BufLen;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

    rStatus = kalIoctl(prGlueInfo,
        wlanoidSetDisassociate,
        NULL,
        0,
        FALSE,
        FALSE,
        TRUE,
        FALSE,
        &u4BufLen);

    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(REQ, WARN, ("disassociate error:%x\n", rStatus));
        return -EFAULT;
    }

    return 0;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to join an IBSS group
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_join_ibss (
    struct wiphy *wiphy,
    struct net_device *ndev,
    struct cfg80211_ibss_params *params
    )
{
    PARAM_SSID_T rNewSsid;
    P_GLUE_INFO_T prGlueInfo = NULL;
    UINT_32 u4ChnlFreq; /* Store channel or frequency information */
    UINT_32 u4BufLen = 0;
    WLAN_STATUS rStatus;
    struct ieee80211_channel *channel = NULL;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

    /* set channel */
    if(params->chandef.chan)
    	channel = params->chandef.chan;
    if(channel) {
        u4ChnlFreq = nicChannelNum2Freq(channel->hw_value);
        rStatus = kalIoctl(prGlueInfo,
                           wlanoidSetFrequency,
                           &u4ChnlFreq,
                           sizeof(u4ChnlFreq),
                           FALSE,
                           FALSE,
                           FALSE,
                           FALSE,
                           &u4BufLen);
        if (rStatus != WLAN_STATUS_SUCCESS) {
            return -EFAULT;
        }
    }

    /* set SSID */
    kalMemCopy(rNewSsid.aucSsid, params->ssid, params->ssid_len);
    rStatus = kalIoctl(prGlueInfo,
            wlanoidSetSsid,
            (PVOID) &rNewSsid,
            sizeof(PARAM_SSID_T),
            FALSE,
            FALSE,
            TRUE,
            FALSE,
            &u4BufLen);

    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(REQ, WARN, ("set SSID:%x\n", rStatus));
        return -EFAULT;
    }

    return 0;


    return -EINVAL;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to leave from IBSS group
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_leave_ibss (
    struct wiphy *wiphy,
    struct net_device *ndev
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS rStatus;
    UINT_32 u4BufLen;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

    rStatus = kalIoctl(prGlueInfo,
        wlanoidSetDisassociate,
        NULL,
        0,
        FALSE,
        FALSE,
        TRUE,
        FALSE,
        &u4BufLen);

    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(REQ, WARN, ("disassociate error:%x\n", rStatus));
        return -EFAULT;
    }

    return 0;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to configure 
 *        WLAN power managemenet
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_set_power_mgmt (
    struct wiphy *wiphy,
    struct net_device *ndev,
    bool enabled,
    int timeout
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS rStatus;
    UINT_32 u4BufLen;
    PARAM_POWER_MODE ePowerMode;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

    if(enabled) {
        if(timeout == -1) {
            ePowerMode = Param_PowerModeFast_PSP;
        }
        else {
            ePowerMode = Param_PowerModeMAX_PSP;
        }
    }
    else {
        ePowerMode = Param_PowerModeCAM;
    }

    rStatus = kalIoctl(prGlueInfo,
        wlanoidSet802dot11PowerSaveProfile,
        &ePowerMode,
        sizeof(ePowerMode),
        FALSE,
        FALSE,
        TRUE,
        FALSE,
        &u4BufLen);

    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(REQ, WARN, ("set_power_mgmt error:%x\n", rStatus));
        return -EFAULT;
    }

    return 0;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to cache
 *        a PMKID for a BSSID
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_set_pmksa (
    struct wiphy *wiphy,
    struct net_device *ndev,
    struct cfg80211_pmksa *pmksa
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS    rStatus;
    UINT_32        u4BufLen;
    P_PARAM_PMKID_T  prPmkid;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

    prPmkid =(P_PARAM_PMKID_T)kalMemAlloc(8 + sizeof(PARAM_BSSID_INFO_T), VIR_MEM_TYPE);
    if (!prPmkid) {
        DBGLOG(INIT, INFO, ("Can not alloc memory for IW_PMKSA_ADD\n"));
        return -ENOMEM;
    }
    
    prPmkid->u4Length = 8 + sizeof(PARAM_BSSID_INFO_T);
    prPmkid->u4BSSIDInfoCount = 1;
    kalMemCopy(prPmkid->arBSSIDInfo->arBSSID, pmksa->bssid, 6);
    kalMemCopy(prPmkid->arBSSIDInfo->arPMKID, pmksa->pmkid, IW_PMKID_LEN);
    
    rStatus = kalIoctl(prGlueInfo,
                 wlanoidSetPmkid,
                 prPmkid,
                 sizeof(PARAM_PMKID_T),
                 FALSE,
                 FALSE,
                 FALSE,
                 FALSE,
                 &u4BufLen);
    
    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(INIT, INFO, ("add pmkid error:%x\n", rStatus));
    }
    kalMemFree(prPmkid, VIR_MEM_TYPE, 8 + sizeof(PARAM_BSSID_INFO_T));

    return 0;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to remove
 *        a cached PMKID for a BSSID
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_del_pmksa (
    struct wiphy *wiphy,
    struct net_device *ndev,
    struct cfg80211_pmksa *pmksa
    )
{

    return 0;
}


/*----------------------------------------------------------------------------*/
/*!
 * @brief This routine is responsible for requesting to flush
 *        all cached PMKID
 *
 * @param 
 *
 * @retval 0:       successful
 *         others:  failure
 */
/*----------------------------------------------------------------------------*/
int
mtk_cfg80211_flush_pmksa (
    struct wiphy *wiphy,
    struct net_device *ndev
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    WLAN_STATUS    rStatus;
    UINT_32        u4BufLen;
    P_PARAM_PMKID_T  prPmkid;

    prGlueInfo = (P_GLUE_INFO_T) wiphy_priv(wiphy);
    ASSERT(prGlueInfo);

    prPmkid =(P_PARAM_PMKID_T)kalMemAlloc(8, VIR_MEM_TYPE);
    if (!prPmkid) {
        DBGLOG(INIT, INFO, ("Can not alloc memory for IW_PMKSA_FLUSH\n"));
        return -ENOMEM;
    }
    
    prPmkid->u4Length = 8;
    prPmkid->u4BSSIDInfoCount = 0;
    
    rStatus = kalIoctl(prGlueInfo,
                 wlanoidSetPmkid,
                 prPmkid,
                 sizeof(PARAM_PMKID_T),
                 FALSE,
                 FALSE,
                 FALSE,
                 FALSE,
                 &u4BufLen);
    
    if (rStatus != WLAN_STATUS_SUCCESS) {
        DBGLOG(INIT, INFO, ("flush pmkid error:%x\n", rStatus));
    }
    kalMemFree(prPmkid, VIR_MEM_TYPE, 8);

    return 0;
}

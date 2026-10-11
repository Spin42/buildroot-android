/*! \file   gl_wext.h
    \brief  Information element search helpers shared by the cfg80211 glue.

    The wireless extensions interface is not part of this port; only the IE
    lookup helpers survive, under their historical names.
*/

#ifndef _GL_WEXT_H
#define _GL_WEXT_H

BOOLEAN
wextSrchDesiredWPAIE (
    IN  PUINT_8         pucIEStart,
    IN  INT_32          i4TotalIeLen,
    IN  UINT_8          ucDesiredElemId,
    OUT PUINT_8         *ppucDesiredIE
    );

#if CFG_SUPPORT_WPS
BOOLEAN
wextSrchDesiredWPSIE (
    IN PUINT_8 pucIEStart,
    IN INT_32 i4TotalIeLen,
    IN UINT_8 ucDesiredElemId,
    OUT PUINT_8 *ppucDesiredIE
    );
#endif

#endif /* _GL_WEXT_H */

#ifndef __HCT_BOARD_CONFIG_H__
#define __HCT_BOARD_CONFIG_H__


#undef HCT_YES
#define HCT_YES 1
#undef HCT_NO
#define HCT_NO 0

/*audio releated */
/* phone mic mode */
#define __HCT_PHONE_MIC_MODE__   2
/* accdet mic mode */
#define __HCT_ACCDET_MIC_MODE__  2

#define __HCT_OTG_GPIO_SELECT__  HCT_YES

#define __HCT_TYPEC_ACCDET_MIC_SUPPORT__  HCT_YES
#define __HCT_WIRELESS_CHARGER_WHEN_TPC_IN_SUPPORT__  HCT_YES

 /*phone use exp audio pa*/
#define __HCT_USING_EXTAMP_HP__  HCT_YES

/**###########################audio gpio define##################***/

#if  __HCT_USING_EXTAMP_HP__  
    #define __HCT_EXTAMP_HP_MODE__    3
    #define __HCT_EXTAMP_GPIO_NUM__    136
#endif

#endif

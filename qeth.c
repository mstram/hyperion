/* QETH.C       (c) Copyright Jan Jaeger,   1999-2012                */
/*              OSA Express                                          */
/*                                                                   */

/* This module contains device handling functions for the            */
/* OSA Express emulated card                                         */
/*                                                                   */
/* This implementation is based on the S/390 Linux implementation    */
/*                                                                   */
/* Device module hdtqeth                                             */
/*                                                                   */
/* hercules.cnf:                                                     */
/* 0A00-0A02 QETH <optional parameters>                              */
/* Default parm:   iface /dev/net/tun                                */
/* Optional parms: ifname  <name of interface>                       */
/*                 hwaddr  <MAC address>                             */
/*                 ipaddr  <IPv4 address and prefix length>          */
/*                 netmask <IPv4 netmask>                            */
/*                 ipaddr6 <IPv6 address and prefix length>          */
/*                 mtu     <MTU>                                     */
/*                 chpid   <channel path id>                         */
/*                 debug                                             */
/*                                                                   */
/* When using a bridged configuration no parameters are required     */
/* on the QETH device statement.  The tap device will in that case   */
/* need to be  bridged to another virtual or real ethernet adapter.  */
/* e.g.                                                              */
/* 0A00.3 QETH                                                       */
/* The tap device will need to be bridged e.g.                       */
/* brctl addif <bridge> tap0                                         */
/*                                                                   */
/* When using a routed configuration the tap device needs to have    */
/* an IP address assigned in the same subnet as the guests virtual   */
/* eth adapter.                                                      */
/* e.g.                                                              */
/* 0A00.3 QETH ipaddr 192.168.10.1                                   */
/* where the guest can then use any other IP address in the          */
/* 192.168.10 range                                                  */
/*                                                                   */

#include "hstdinc.h"
#include "hercules.h"
#include "devtype.h"
#include "chsc.h"
#include "mpc.h"
#include "tuntap.h"
#include "ctcadpt.h"
#include "hercifc.h"
#include "qeth.h"


/*-------------------------------------------------------------------*/
/* QETH Debugging                                                    */
/*-------------------------------------------------------------------*/

#define ENABLE_QETH_DEBUG   0   // 1:enable, 0:disable, #undef:default
#define QETH_TIMING_DEBUG       // #define to debug speed/performance
//#define QETH_DUMP_DATA          // #undef to suppress i/o buffers dump


/*-------------------------------------------------------------------*/
/* VS 2010 Compiler Crash Workaround                                 */
/*-------------------------------------------------------------------*/
#if defined(_MSVC_)

    /* Keep store_f3 defined as a macro, but redefine to a local
     * variant.
     */
    #undef  store_f3
    #define store_f3(_ptr,_value) store_f3_local((_ptr),(_value))

    /* Local definition for store_f3 */
    static __inline__ void store_f3_local(void *ptr, const U32 value)
    {
        U32 temp = CSWAP32(value) >> 8;
        memcpy((BYTE *)ptr, (BYTE *)&temp, 3);
    }

#endif


/*-------------------------------------------------------------------*/
/* QETH Debugging                                                    */
/*-------------------------------------------------------------------*/

/* (enable debugging if needed/requested) */
#if (!defined(ENABLE_QETH_DEBUG) && defined(DEBUG)) ||  \
    ( defined(ENABLE_QETH_DEBUG) && ENABLE_QETH_DEBUG)
  #define QETH_DEBUG
#endif

/* (activate debugging if debugging is enabled) */
#if defined(QETH_DEBUG)
  #define  ENABLE_TRACING_STMTS   1     // (Fish: DEBUGGING)
  #include "dbgtrace.h"                 // (Fish: DEBUGGING)
  #define  NO_QETH_OPTIMIZE             // (Fish: DEBUGGING) (MSVC only)
  #define  QETH_TIMING_DEBUG            // (Fish: DEBUG speed/timing)
#endif

/* (disable optimizations if debugging) */
#if defined( _MSVC_ ) && defined( NO_QETH_OPTIMIZE )
  #pragma optimize( "", off )           // disable optimizations for reliable breakpoints
#endif

/* (debug QETH speed/performance issues) */
#if defined( QETH_TIMING_DEBUG ) || defined( OPTION_WTHREADS )
  #define PTT_QETH_TIMING_DEBUG( _class, _string, _tr1, _tr2, _tr3) \
                            PTT( _class, _string, _tr1, _tr2, _tr3)
#else
  #define PTT_QETH_TIMING_DEBUG( _class, _string, _tr1, _tr2, _tr3)
#endif

/* (activate DBGTRC statements if needed) */
#if defined(QETH_DEBUG)
  #define DBGTRC(_dev, ...)                 \
    do {                                    \
      DEVGRP *devgrp = (_dev)->group;       \
      if (devgrp) {                         \
        OSA_GRP* grp = devgrp->grp_data;    \
        if(grp && grp->debug)               \
          TRACE("QETH: " __VA_ARGS__);      \
      }                                     \
    } while(0)
  #define DBGTRC2(_dev, _msg, ...)          \
    do {                                    \
      DEVGRP *devgrp = (_dev)->group;       \
      if (devgrp) {                         \
        OSA_GRP* grp = devgrp->grp_data;    \
        if(grp && grp->debug) {             \
          char buf[256];                    \
          MSGBUF(buf,_msg,__VA_ARGS__);     \
          TRACE("QETH: %s", buf);           \
        }                                   \
      }                                     \
    } while(0)
#else
  #define DBGTRC(_dev, ...)         do{;}while(0)
  #define DBGTRC2(_dev, _msg, ...)  do{;}while(0)
#endif

/* (activate tracing of I/O data buffers) */
#if defined( QETH_DUMP_DATA )
  #define MPC_DUMP_DATA(_msg,_adr,_len,_dir)  \
    mpc_display_stuff( dev, _msg, _adr, _len, _dir )
#else
  #define MPC_DUMP_DATA(_msg,_adr,_len,_dir)    do{;}while(0)
#endif // QETH_DUMP_DATA


/*-------------------------------------------------------------------*/
/* Hercules Dynamic Loader (HDL)                                     */
/*-------------------------------------------------------------------*/
#if defined( OPTION_DYNAMIC_LOAD )
  #if defined( WIN32 ) && !defined( _MSVC_ ) && !defined( HDL_USE_LIBTOOL )
    SYSBLK *psysblk;
    #define sysblk (*psysblk)
  #endif
#endif /*defined( OPTION_DYNAMIC_LOAD )*/


/*-------------------------------------------------------------------*/
/* Functions, entirely internal to qeth.c                            */
/*-------------------------------------------------------------------*/
OSA_BHR* process_cm_enable( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );
OSA_BHR* process_cm_setup( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );
OSA_BHR* process_cm_takedown( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );
OSA_BHR* process_cm_disable( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );
int      process_ulp_enable_extract( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );
OSA_BHR* process_ulp_enable( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );
OSA_BHR* process_ulp_setup( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );
OSA_BHR* process_dm_act( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );
OSA_BHR* process_ulp_takedown( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );
OSA_BHR* process_ulp_disable( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );
OSA_BHR* process_unknown_puk( DEVBLK*, MPC_TH*, MPC_RRH*, MPC_PUK* );

OSA_BHR* alloc_buffer( DEVBLK*, int );
void*    add_buffer_to_chain_and_signal_event( OSA_GRP*, OSA_BHR* );
void*    add_buffer_to_chain( OSA_GRP*, OSA_BHR* );
OSA_BHR* remove_buffer_from_chain( OSA_GRP* );
void*    remove_and_free_any_buffers_on_chain( OSA_GRP* );

/*-------------------------------------------------------------------*/
/* Internal helper functions                                         */
/*-------------------------------------------------------------------*/
void InitMACAddr( DEVBLK* dev, OSA_GRP* grp );
void InitMTU    ( DEVBLK* dev, OSA_GRP* grp );

/*-------------------------------------------------------------------*/
/* Configuration Data Constants                                      */
/*-------------------------------------------------------------------*/
static const NED  osa_device_ned[]  = {OSA_DEVICE_NED};
static const NED  osa_ctlunit_ned[] = {OSA_CTLUNIT_NED};
static const NED  osa_token_ned[]   = {OSA_TOKEN_NED};
static const NEQ  osa_general_neq[] = {OSA_GENERAL_NEQ};

static NED configuration_data[4]; // (initialized by HDL_DEPENDENCY_SECTION)

static const ND  osa_nd[] = {OSA_ND};
static const NQ  osa_nq[] = {OSA_NQ};

static ND node_data[2]; // (initialized by HDL_DEPENDENCY_SECTION)

#define SII_SIZE    sizeof(U32)

static const BYTE sense_id_bytes[] =
{
    0xFF,                               /* Always 0xFF               */
    OSA_SNSID_1731_01,                  /* Control Unit type/model   */
    OSA_SNSID_1732_01,                  /* I/O Device   type/model   */
    0x00,                               /* Always 0x00               */
    OSA_RCD_CIW,                        /* Read Config. Data CIW     */
    OSA_SII_CIW,                        /* Set Interface Id. CIW     */
    OSA_RNI_CIW,                        /* Read Node Identifier CIW  */
    OSA_EQ_CIW,                         /* Establish Queues CIW      */
    OSA_AQ_CIW                          /* Activate Queues CIW       */
};


static BYTE qeth_immed_commands [256] =
{
/* 0 1 2 3 4 5 6 7 8 9 A B C D E F */
   0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0, /* 00 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 10 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 20 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 30 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 40 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 50 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 60 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 70 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 80 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 90 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* A0 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* B0 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* C0 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* D0 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* E0 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0  /* F0 */
};


/*-------------------------------------------------------------------*/
/* STORCHK macro: check storage access & update ref & change bits.   */
/* Returns 0 if successful or CSW_PROGC or CSW_PROTC if error.       */
/* Storage key ref & change bits are only updated if successful.     */
/*-------------------------------------------------------------------*/
#define STORCHK(_addr,_len,_key,_acc,_dev) \
  (((((_addr) + (_len)) > (_dev)->mainlim) \
    || (((_dev)->orb.flag5 & ORB5_A) \
      && ((((_dev)->pmcw.flag5 & PMCW5_LM_LOW)  \
        && ((_addr) < sysblk.addrlimval)) \
      || (((_dev)->pmcw.flag5 & PMCW5_LM_HIGH) \
        && (((_addr) + (_len)) > sysblk.addrlimval)) ) )) ? CSW_PROGC : \
   ((_key) && ((STORAGE_KEY((_addr), (_dev)) & STORKEY_KEY) != (_key)) \
&& ((STORAGE_KEY((_addr), (_dev)) & STORKEY_FETCH) || ((_acc) == STORKEY_CHANGE))) ? CSW_PROTC : \
  ((STORAGE_KEY((_addr), (_dev)) |= ((((_acc) == STORKEY_CHANGE)) \
    ? (STORKEY_REF|STORKEY_CHANGE) : STORKEY_REF)) && 0))


/*-------------------------------------------------------------------*/
/* Register local MAC address                                        */
/*-------------------------------------------------------------------*/
static inline int register_mac(BYTE *mac, int type, OSA_GRP *grp)
{
int i;
    for(i = 0; i < OSA_MAXMAC; i++)
        if(!grp->mac[i].type || !memcmp(grp->mac[i].addr,mac,IFHWADDRLEN))
        {
            memcpy(grp->mac[i].addr,mac,IFHWADDRLEN);
            grp->mac[i].type = type;
            return type;
        }
    return MAC_TYPE_NONE;
}


/*-------------------------------------------------------------------*/
/* Deregister local MAC address                                      */
/*-------------------------------------------------------------------*/
static inline int deregister_mac(BYTE *mac, int type, OSA_GRP *grp)
{
int i;
    for(i = 0; i < OSA_MAXMAC; i++)
        if((grp->mac[i].type == type) && !memcmp(grp->mac[i].addr,mac,IFHWADDRLEN))
        {
            grp->mac[i].type = MAC_TYPE_NONE;
            return type;
        }
    return MAC_TYPE_NONE;
}


/*-------------------------------------------------------------------*/
/* Validate MAC address and return MAC type                          */
/*-------------------------------------------------------------------*/
static inline int validate_mac(BYTE *mac, int type, OSA_GRP *grp)
{
int i;
    for(i = 0; i < OSA_MAXMAC; i++)
    {
        if((grp->mac[i].type & type) && !memcmp(grp->mac[i].addr,mac,IFHWADDRLEN))
            return grp->mac[i].type | grp->promisc;
    }
    return grp->promisc;
}


#if defined(_FEATURE_QDIO_THININT)
/*-------------------------------------------------------------------*/
/* Set Adapter Local Summary Indicator bits                          */
/*-------------------------------------------------------------------*/
static inline void set_alsi(DEVBLK *dev, BYTE bits)
{
    if(dev->qdio.alsi)
    {
    BYTE *alsi = dev->mainstor + dev->qdio.alsi;

        obtain_lock(&sysblk.mainlock);
        *alsi |= bits;
        STORAGE_KEY(dev->qdio.alsi, dev) |= (STORKEY_REF|STORKEY_CHANGE);
        release_lock(&sysblk.mainlock);
    }
}
#define SET_ALSI(_dev,_bits)    set_alsi((_dev),(_bits))


/*-------------------------------------------------------------------*/
/* Clear Adapter Local Summary Indicator bits                        */
/*-------------------------------------------------------------------*/
static inline void clr_alsi(DEVBLK *dev, BYTE bits)
{
    if(dev->qdio.alsi)
    {
    BYTE *alsi = dev->mainstor + dev->qdio.alsi;

        obtain_lock(&sysblk.mainlock);
        *alsi &= bits;
        STORAGE_KEY(dev->qdio.alsi, dev) |= (STORKEY_REF|STORKEY_CHANGE);
        release_lock(&sysblk.mainlock);
    }
}
#define CLR_ALSI(_dev,_bits)    clr_alsi((_dev),(_bits))


/*-------------------------------------------------------------------*/
/* Set Device State Change Indicator bits                            */
/*-------------------------------------------------------------------*/
static inline void set_dsci(DEVBLK *dev, BYTE bits)
{
    if(dev->qdio.dsci)
    {
    BYTE *dsci = dev->mainstor + dev->qdio.dsci;
    BYTE *alsi = dev->mainstor + dev->qdio.alsi;

        obtain_lock(&sysblk.mainlock);
        *dsci |= bits;
        STORAGE_KEY(dev->qdio.dsci, dev) |= (STORKEY_REF|STORKEY_CHANGE);
        *alsi |= bits;
        STORAGE_KEY(dev->qdio.alsi, dev) |= (STORKEY_REF|STORKEY_CHANGE);
        release_lock(&sysblk.mainlock);
    }
}
#define SET_DSCI(_dev,_bits)    set_dsci((_dev),(_bits))


/*-------------------------------------------------------------------*/
/* Clear Device State Change Indicator bits                          */
/*-------------------------------------------------------------------*/
static inline void clr_dsci(DEVBLK *dev, BYTE bits)
{
    if(dev->qdio.dsci)
    {
    BYTE *dsci = dev->mainstor + dev->qdio.dsci;

        obtain_lock(&sysblk.mainlock);
        *dsci &= bits;
        STORAGE_KEY(dev->qdio.dsci, dev) |= (STORKEY_REF|STORKEY_CHANGE);
        release_lock(&sysblk.mainlock);
    }
}
#define CLR_DSCI(_dev,_bits)    clr_dsci((_dev),(_bits))

#else /*!defined(_FEATURE_QDIO_THININT)*/

#define SET_ALSI(_dev,_bits)    /* (do nothing) */
#define CLR_ALSI(_dev,_bits)    /* (do nothing) */
#define SET_DSCI(_dev,_bits)    /* (do nothing) */
#define CLR_DSCI(_dev,_bits)    /* (do nothing) */

#endif /*defined(_FEATURE_QDIO_THININT)*/


/*-------------------------------------------------------------------*/
/* Issue generic error message with return code and strerror msg.    */
/* Returns the same errnum value that was passed.                    */
/*-------------------------------------------------------------------*/
static int qeth_errnum_msg(DEVBLK *dev, OSA_GRP *grp,
                            int errnum, char* msgcode, char* errmsg )
{
    char strerr[256] = {0};
    char msgbuf[256] = {0};

    if (errnum >= 0)
        strlcpy( strerr, strerror( errnum ), sizeof( strerr ));
    else
        strlcpy( strerr, "An unidentified error has occurred", sizeof( strerr ));

    /* "function() failed, rc=99 (0x00000063): an error occurred" */
    MSGBUF( msgbuf, "%s, rc=%d (0x%08X): %s",
        errmsg, errnum, errnum, strerr);

    // HHC03996 "%1d:%04X %s: %s: %s"
    if (str_caseless_eq("E",msgcode))
        WRMSG( HHC03996, "E", SSID_TO_LCSS(dev->ssid), dev->devnum,
            "QETH", grp->ttifname, msgbuf);
    else if (str_caseless_eq("W",msgcode))
        WRMSG( HHC03996, "W", SSID_TO_LCSS(dev->ssid), dev->devnum,
            "QETH", grp->ttifname, msgbuf);
    else /* "I" information presumed */
        WRMSG( HHC03996, "I", SSID_TO_LCSS(dev->ssid), dev->devnum,
            "QETH", grp->ttifname, msgbuf);
    return errnum;
}


/*-------------------------------------------------------------------*/
/* Report what values we are using                                   */
/*-------------------------------------------------------------------*/
static void qeth_report_using (DEVBLK *dev, OSA_GRP *grp, int enabled)
{
    char not[8];
    strlcpy( not, enabled ? "" : "not ", sizeof( not ));

    WRMSG( HHC03997, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, "QETH",
        grp->ttifname, not, "MAC", grp->tthwaddr );

    if (grp->ttipaddr)
    {
        WRMSG( HHC03997, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, "QETH",
            grp->ttifname, not, "IPv4", grp->ttipaddr );
        WRMSG( HHC03997, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, "QETH",
            grp->ttifname, not, "MASK", grp->ttnetmask );
    }

    if (grp->ttipaddr6)
        WRMSG( HHC03997, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, "QETH",
            grp->ttifname, not, "IPv6", grp->ttipaddr6 );

    WRMSG( HHC03997, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, "QETH",
        grp->ttifname, not, "MTU", grp->ttmtu );
}


/*-------------------------------------------------------------------*/
/* Enable the TUNTAP interface  (set IFF_UP flag)                    */
/*-------------------------------------------------------------------*/
static int qeth_enable_interface (DEVBLK *dev, OSA_GRP *grp)
{
    int rc;

    if (grp->enabled)
        return 0;

    if ((rc = TUNTAP_SetFlags( grp->ttifname, 0
        | IFF_UP
        | IFF_MULTICAST
        | IFF_BROADCAST
#if defined( TUNTAP_IFF_RUNNING_NEEDED )
        | IFF_RUNNING
#endif /* defined( TUNTAP_IFF_RUNNING_NEEDED ) */
#if defined(QETH_DEBUG) || defined(IFF_DEBUG)
        | (grp->debug ? IFF_DEBUG : 0)
#endif /*defined(QETH_DEBUG) || defined(IFF_DEBUG)*/
        | (grp->promisc ? IFF_PROMISC : 0)
    )) != 0)
        qeth_errnum_msg( dev, grp, rc,
            "E", "qeth_enable_interface() failed" );
    grp->enabled = 1;
    return rc;
}


/*-------------------------------------------------------------------*/
/* Disable the TUNTAP interface  (clear IFF_UP flag)                 */
/*-------------------------------------------------------------------*/
static int qeth_disable_interface (DEVBLK *dev, OSA_GRP *grp)
{
    int rc;

    if (!grp->enabled)
        return 0;

    if ((rc = TUNTAP_SetFlags( grp->ttifname, 0
        | IFF_MULTICAST
        | IFF_BROADCAST
#if defined( TUNTAP_IFF_RUNNING_NEEDED )
        | IFF_RUNNING
#endif /* defined( TUNTAP_IFF_RUNNING_NEEDED ) */
#if defined(QETH_DEBUG) || defined(IFF_DEBUG)
        | (grp->debug ? IFF_DEBUG : 0)
#endif /*defined(QETH_DEBUG) || defined(IFF_DEBUG)*/
        | (grp->promisc ? IFF_PROMISC : 0)
    )) != 0)
        qeth_errnum_msg( dev, grp, rc,
            "E", "qeth_disable_interface() failed" );

    grp->enabled = 0;
    return rc;
}


/*-------------------------------------------------------------------*/
/* Create the TUNTAP interface                                       */
/*-------------------------------------------------------------------*/
static int qeth_create_interface (DEVBLK *dev, OSA_GRP *grp)
{
    int i, rc;

    /* Discard the old interface by closing the TUNTAP device */
    if (grp->ttfd >= 0)
    {
        int ttfd = grp->ttfd;
        grp->ttfd = -1;
        for (i=0; i < dev->group->acount; i++)
            dev->group->memdev[i]->fd = -1;
        TUNTAP_Close( ttfd );
    }

    /* Create the new interface by opening the TUNTAP device */
    if ((rc = TUNTAP_CreateInterface
    (
        grp->tuntap,
        0
            | IFF_NO_PI
            | IFF_OSOCK
            | (grp->l3 ? IFF_TUN : IFF_TAP)
        ,
        &grp->ttfd,
        grp->ttifname

    )) != 0)
        return qeth_errnum_msg( dev, grp, rc,
            "E", "TUNTAP_CreateInterface() failed" );

    /* Update DEVBLK file descriptors */
    for (i=0; i < dev->group->acount; i++)
        dev->group->memdev[i]->fd = grp->ttfd;

    // HHC00901 "%1d:%04X %s: interface %s, type %s opened"
    WRMSG( HHC00901, "I", SSID_TO_LCSS(dev->ssid),
                         dev->devnum,
                         dev->typname,
                         grp->ttifname,
                         (grp->l3 ? "TUN" : "TAP"));

    /* Set NON-Blocking mode by disabling Blocking mode */
    if ((rc = socket_set_blocking_mode(grp->ttfd,0)) != 0)
        qeth_errnum_msg( dev, grp, rc,
            "W", "socket_set_blocking_mode() failed" );

    /* Make sure the interface has a valid MAC address */
    if (grp->tthwaddr) {
#if defined( OPTION_TUNTAP_SETMACADDR )
        if ((rc = TUNTAP_SetMACAddr(grp->ttifname,grp->tthwaddr)) != 0)
            return qeth_errnum_msg( dev, grp, rc,
                "E", "TUNTAP_SetMACAddr() failed" );
#endif /*defined( OPTION_TUNTAP_SETMACADDR )*/
    } else {
        InitMACAddr( dev, grp );
    }

    /* If possible, assign an IPv4 address to the interface */
    if(grp->ttipaddr)
#if defined( OPTION_W32_CTCI )
        if ((rc = TUNTAP_SetDestAddr(grp->ttifname,grp->ttipaddr)) != 0)
            return qeth_errnum_msg( dev, grp, rc,
                "E", "TUNTAP_SetDestAddr() failed" );
#else /*!defined( OPTION_W32_CTCI )*/
        if ((rc = TUNTAP_SetIPAddr(grp->ttifname,grp->ttipaddr)) != 0)
            return qeth_errnum_msg( dev, grp, rc,
                "E", "TUNTAP_SetIPAddr() failed" );
#endif /*defined( OPTION_W32_CTCI )*/

    /* Same thing with the IPv4 subnet mask */
#if defined( OPTION_TUNTAP_SETNETMASK )
    if(grp->ttnetmask)
        if ((rc = TUNTAP_SetNetMask(grp->ttifname,grp->ttnetmask)) != 0)
            return qeth_errnum_msg( dev, grp, rc,
                "E", "TUNTAP_SetNetMask() failed" );
#endif /*defined( OPTION_TUNTAP_SETNETMASK )*/

    /* Assign it an IPv6 address too, if possible */
#if defined(ENABLE_IPV6)
    if(grp->ttipaddr6)
        if((rc = TUNTAP_SetIPAddr6(grp->ttifname, grp->ttipaddr6, grp->ttpfxlen6)) != 0)
            return qeth_errnum_msg( dev, grp, rc,
                "E", "TUNTAP_SetIPAddr6() failed" );
#endif /*defined(ENABLE_IPV6)*/

    /* Set the interface's MTU size */
    if (grp->ttmtu) {
        if ((rc = TUNTAP_SetMTU(grp->ttifname,grp->ttmtu)) != 0) {
            return qeth_errnum_msg( dev, grp, rc,
                "E", "TUNTAP_SetMTU() failed" );
        }
        grp->uMTU = (U16) atoi( grp->ttmtu );
    } else {
        InitMTU( dev, grp );
    }

    /* Enable the interface */
    if ((rc = TUNTAP_SetFlags( grp->ttifname, 0
        | IFF_UP
        | IFF_MULTICAST
        | IFF_BROADCAST
#if defined( TUNTAP_IFF_RUNNING_NEEDED )
        | IFF_RUNNING
#endif /* defined( TUNTAP_IFF_RUNNING_NEEDED ) */
#if defined(QETH_DEBUG) || defined(IFF_DEBUG)
        | (grp->debug ? IFF_DEBUG : 0)
#endif /*defined(QETH_DEBUG) || defined(IFF_DEBUG)*/
        | (grp->promisc ? IFF_PROMISC : 0)
    )) != 0)
        return qeth_errnum_msg( dev, grp, rc,
            "E", "TUNTAP_SetFlags failed" );

    return 0;
}


/*-------------------------------------------------------------------*/
/* Adapter Command Routine                                           */
/*-------------------------------------------------------------------*/
static void osa_adapter_cmd(DEVBLK *dev, MPC_TH *req_th)
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

OSA_BHR *rsp_bhr;

MPC_RRH *req_rrh;
MPC_PH  *req_ph;

U32 offrrh;
U16 offph;

    /* Point to request MPC_RRH and MPC_PH. */
    FETCH_FW(offrrh,req_th->offrrh);
    req_rrh = (MPC_RRH*)((BYTE*)req_th+offrrh);
    FETCH_HW(offph,req_rrh->offph);
    req_ph = (MPC_PH*)((BYTE*)req_rrh+offph);

    switch(req_rrh->type) {

    case RRH_TYPE_CM:
        {
            MPC_PUK *req_puk;

            req_puk = mpc_point_puk( dev, req_th, req_rrh );

            switch(req_puk->type) {

            case PUK_TYPE_ENABLE:
                rsp_bhr = process_cm_enable( dev, req_th, req_rrh, req_puk );
                break;

            case PUK_TYPE_SETUP:
                rsp_bhr = process_cm_setup( dev, req_th, req_rrh, req_puk );
                break;

            case PUK_TYPE_TAKEDOWN:
                rsp_bhr = process_cm_takedown( dev, req_th, req_rrh, req_puk );
                break;

            case PUK_TYPE_DISABLE:
                rsp_bhr = process_cm_disable( dev, req_th, req_rrh, req_puk );
                break;

            default:
                rsp_bhr = process_unknown_puk( dev, req_th, req_rrh, req_puk );

            }

            // Add response buffer to chain.
            add_buffer_to_chain_and_signal_event( grp, rsp_bhr );

        }
        break;

    case RRH_TYPE_ULP:
        {
            MPC_PUK *req_puk;

            req_puk = mpc_point_puk(dev,req_th,req_rrh);

            switch(req_puk->type) {

            case PUK_TYPE_ENABLE:
                if (process_ulp_enable_extract( dev, req_th, req_rrh, req_puk ) != 0)
                {
                    rsp_bhr = NULL;
                    break;
                }
                if (qeth_create_interface( dev, grp ) != 0)
                    qeth_errnum_msg( dev, grp, -1,
                        "E", "qeth_create_interface() failed" );
                rsp_bhr = process_ulp_enable( dev, req_th, req_rrh, req_puk );
                break;

            case PUK_TYPE_SETUP:
                rsp_bhr = process_ulp_setup( dev, req_th, req_rrh, req_puk );
                break;

            case PUK_TYPE_ACTIVE:
                rsp_bhr = process_dm_act( dev, req_th, req_rrh, req_puk );
                break;

            case PUK_TYPE_TAKEDOWN:
                rsp_bhr = process_ulp_takedown( dev, req_th, req_rrh, req_puk );
                break;

            case PUK_TYPE_DISABLE:
                rsp_bhr = process_ulp_disable( dev, req_th, req_rrh, req_puk );
                break;

            default:
                rsp_bhr = process_unknown_puk( dev, req_th, req_rrh, req_puk );
            }

            // Add response buffer to chain.
            add_buffer_to_chain_and_signal_event( grp, rsp_bhr );
        }
        break;

    case RRH_TYPE_IPA:
        {
            MPC_TH  *rsp_th;
            MPC_RRH *rsp_rrh;
            MPC_PH  *rsp_ph;
            MPC_IPA *ipa;

            U32      rqsize;
            U32      offdata;
            U32      lendata;
//          U32      ackseq;

            /* Allocate a buffer to which the request will be copied */
            /* and then modified, to become the response.            */
            FETCH_FW(rqsize,req_th->length);
            rsp_bhr = alloc_buffer( dev, rqsize+100 );
            if (!rsp_bhr)
                break;
            rsp_bhr->datalen = rqsize;

            /* Point to response MPC_TH. */
            rsp_th = (MPC_TH*)((BYTE*)rsp_bhr + SizeBHR);

            /* Copy request to response buffer. */
            memcpy(rsp_th,req_th,rqsize);

            /* Point to response MPC_RRH and MPC_PH. */
            rsp_rrh = (MPC_RRH*)((BYTE*)rsp_th+offrrh);
            rsp_ph = (MPC_PH*)((BYTE*)rsp_rrh+offph);

            /* Get the length of and point to response MPC_IPA and associated command. */
            FETCH_F3( lendata, rsp_ph->lendata );
            FETCH_FW( offdata, rsp_ph->offdata );
            ipa = (MPC_IPA*)((BYTE*)rsp_th + offdata);

            /* Modify the response MPC_TH and MPC_RRH. */
            STORE_FW( rsp_th->seqnum, 0 );
            STORE_HW( rsp_th->unknown10, 0x0FFC );        /* !!! */
            rsp_rrh->proto = PROTOCOL_UNKNOWN;
            memcpy( rsp_rrh->token, grp->gtulpconn, MPC_TOKEN_LENGTH );

            switch(ipa->cmd) {

            case IPA_CMD_STARTLAN:  /* 0x01 */
                /* Note: the MPC_IPA may be 16-bytes in length, not 20-bytes. */
                {
                U32  uLoselen;
                U32  uLength1;
                U32  uLength3;

                    DBGTRC(dev, "STARTLAN\n");

                    if (lendata > SIZE_IPA_SHORT) {
                        uLoselen = lendata - SIZE_IPA_SHORT;
                        uLength3 = SIZE_IPA_SHORT;
                        uLength1 = rqsize - uLoselen;
                        rsp_bhr->datalen = uLength1;
                        STORE_FW( rsp_th->length, uLength1 );
                        STORE_HW( rsp_rrh->lenfida, (U16)uLength3 );
                        STORE_F3( rsp_rrh->lenalda, uLength3 );
                        STORE_F3( rsp_ph->lendata, uLength3 );
                    }

                    if (!qeth_enable_interface( dev, grp ))
                    {
                        STORE_HW(ipa->rc,IPA_RC_OK);
                        grp->ipae |= IPA_SETADAPTERPARMS;
                        qeth_report_using( dev, grp, 1 );
                    }
                    else
                        STORE_HW(ipa->rc,IPA_RC_FFFF);
                }
                break;

            case IPA_CMD_STOPLAN:  /* 0x02 */
                {
                    DBGTRC(dev, "STOPLAN\n");

                    if (!qeth_disable_interface( dev, grp ))
                    {
                        STORE_HW(ipa->rc,IPA_RC_OK);
                        grp->ipae &= ~IPA_SETADAPTERPARMS;
                        qeth_report_using( dev, grp, 0 );
                    }
                    else
                        STORE_HW(ipa->rc,IPA_RC_FFFF);
                }
                break;

            case IPA_CMD_SETADPPARMS:  /* 0xB8 */
                {
                MPC_IPA_SAP *sap = (MPC_IPA_SAP*)(ipa+1);
                U32 cmd;

                    FETCH_FW(cmd,sap->cmd);
                    DBGTRC(dev, "SETADPPARMS (Set Adapter Parameters: %8.8x)\n",cmd);

                    switch(cmd) {

                    case IPA_SAP_QUERY:  /*0x00000001 */
                        {
                        SAP_QRY *qry = (SAP_QRY*)(sap+1);
                            DBGTRC(dev, "Query SubCommands\n");
                            STORE_FW(qry->suppcm,IPA_SAP_SUPP);
// STORE_FW(qry->suppcm, 0xFFFFFFFF); /* ZZ */
                            STORE_HW(sap->rc,IPA_RC_OK);
                            STORE_HW(ipa->rc,IPA_RC_OK);
                        }
                        break;

                    case IPA_SAP_SETMAC:     /* 0x00000002 */
                        {
                        SAP_SMA *sma = (SAP_SMA*)(sap+1);
                        U32 cmd;

                            FETCH_FW(cmd,sma->cmd);
                            switch(cmd) {

                            case IPA_SAP_SMA_CMD_READ:  /* 0 */
                                DBGTRC(dev, "SETMAC Read MAC address\n");
                                STORE_FW(sap->suppcm,0x93020000);   /* !!!! */
                                STORE_FW(sap->resv004,0x93020000);  /* !!!! */
                                STORE_FW(sma->asize,IFHWADDRLEN);
                                STORE_FW(sma->nomacs,1);
                                memcpy(sma->addr, grp->iMAC, IFHWADDRLEN);
                                STORE_HW(sap->rc,IPA_RC_OK);
                                STORE_HW(ipa->rc,IPA_RC_OK);
                                break;

//                          case IPA_SAP_SMA_CMD_REPLACE:  /* 1 */
//                          case IPA_SAP_SMA_CMD_ADD:  /* 2 */
//                          case IPA_SAP_SMA_CMD_DEL:  /* 4 */
//                          case IPA_SAP_SMA_CMD_RESET:  /* 8 */

                            default:
                                DBGTRC(dev, "SETMAC unsupported command (%08x)\n",cmd);
                                STORE_HW(sap->rc,IPA_RC_UNSUPPORTED_SUBCMD);
                                STORE_HW(ipa->rc,IPA_RC_UNSUPPORTED_SUBCMD);
                            }
                        }
                        break;

                    case IPA_SAP_PROMISC:    /* 0x00000800 */
                        {
                        SAP_SPM *spm = (SAP_SPM*)(sap+1);
                        U32 promisc;
                            FETCH_FW(promisc,spm->promisc);
                            grp->promisc = promisc ? MAC_PROMISC : 0;
                            DBGTRC(dev, "Set Promiscous Mode %s\n",grp->promisc ? "On" : "Off");
                            STORE_HW(sap->rc,IPA_RC_OK);
                            STORE_HW(ipa->rc,IPA_RC_OK);
                        }
                        break;

                    case IPA_SAP_SETACCESS:  /* 0x00010000 */
                        DBGTRC(dev, "Set Access\n");
                        STORE_HW(sap->rc,IPA_RC_OK);
                        STORE_HW(ipa->rc,IPA_RC_OK);
                        break;

                    default:
                        DBGTRC(dev, "Invalid SetAdapter SubCmd(%08x)\n",cmd);
                        STORE_HW(sap->rc,IPA_RC_UNSUPPORTED_SUBCMD);
                        STORE_HW(ipa->rc,IPA_RC_UNSUPPORTED_SUBCMD);

                    }

                }
                /* end case IPA_CMD_SETADPPARMS: */
                break;

            case IPA_CMD_SETVMAC:  /* 0x25 */
                {
                MPC_IPA_MAC *ipa_mac = (MPC_IPA_MAC*)(ipa+1);
                char tthwaddr[32] = {0}; // 11:22:33:44:55:66
                int rc = 0;
                int was_enabled;

                    DBGTRC(dev, "Set VMAC\n");

                    MSGBUF( tthwaddr, "%02X:%02X:%02X:%02X:%02X:%02X"
                        ,ipa_mac->macaddr[0]
                        ,ipa_mac->macaddr[1]
                        ,ipa_mac->macaddr[2]
                        ,ipa_mac->macaddr[3]
                        ,ipa_mac->macaddr[4]
                        ,ipa_mac->macaddr[5]
                    );

                    /* PROGRAMMING NOTE: cannot change the interface
                       once it has been enabled. Thus we temporarily
                       disable it, make our changes, and then enable
                       it again.
                    */
                    if ((was_enabled = grp->enabled))
                        qeth_disable_interface( dev, grp );

                    if ((rc = TUNTAP_SetMACAddr( grp->ttifname, tthwaddr )) != 0)
                    {
                        qeth_errnum_msg( dev, grp, rc,
                            "E", "IPA_CMD_SETVMAC failed" );
                    }

                    if (was_enabled)
                        qeth_enable_interface( dev, grp );

                    if (rc != 0)
                        STORE_HW(ipa->rc,IPA_RC_FFFF);
                    else
                    {
                        if (grp->tthwaddr)
                            free( grp->tthwaddr );

                        grp->tthwaddr = strdup( tthwaddr );
                        memcpy( grp->iMAC, ipa_mac->macaddr, IFHWADDRLEN );
                        qeth_report_using( dev, grp, 1 );

                        if(register_mac(ipa_mac->macaddr,MAC_TYPE_UNICST,grp))
                            STORE_HW(ipa->rc,IPA_RC_OK);
                        else
                            STORE_HW(ipa->rc,IPA_RC_L2_DUP_MAC);
                    }
                }
                break;

            case IPA_CMD_DELVMAC:  /* 0x26 */
                {
                MPC_IPA_MAC *ipa_mac = (MPC_IPA_MAC*)(ipa+1);

                    DBGTRC(dev, "Del VMAC\n");
                    if(deregister_mac(ipa_mac->macaddr,MAC_TYPE_UNICST,grp))
                        STORE_HW(ipa->rc,IPA_RC_OK);
                    else
                        STORE_HW(ipa->rc,IPA_RC_L2_MAC_NOT_FOUND);
                }
                break;

            case IPA_CMD_SETGMAC:  /* 0x23 */
                {
                MPC_IPA_MAC *ipa_mac = (MPC_IPA_MAC*)(ipa+1);

                    DBGTRC(dev, "Set GMAC\n");
                    if(register_mac(ipa_mac->macaddr,MAC_TYPE_MLTCST,grp))
                        STORE_HW(ipa->rc,IPA_RC_OK);
                    else
                        STORE_HW(ipa->rc,IPA_RC_L2_DUP_MAC);
                }
                break;

            case IPA_CMD_DELGMAC:  /* 0x24 */
                {
                MPC_IPA_MAC *ipa_mac = (MPC_IPA_MAC*)(ipa+1);

                    DBGTRC(dev, "Del GMAC\n");
                    if(deregister_mac(ipa_mac->macaddr,MAC_TYPE_MLTCST,grp))
                        STORE_HW(ipa->rc,IPA_RC_OK);
                    else
                        STORE_HW(ipa->rc,IPA_RC_L2_GMAC_NOT_FOUND);
                }
                break;

            case IPA_CMD_SETIP:  /* 0xB1 */
                {
//!                  BYTE *ip = (BYTE*)(ipa+1);
                U16  proto, retcode;

                    DBGTRC(dev, "SETIP (L3 Set IP)\n");

                    FETCH_HW(proto,ipa->proto);
                    retcode = IPA_RC_OK;

//!                     if (proto == IPA_PROTO_IPV4)
//!                     {
//!                         char ipaddr[16] = {0};
//!                         char ipmask[16] = {0};
//!                         int rc = 0;
//!                         int was_enabled;
//!
//!                         MSGBUF(ipaddr,"%d.%d.%d.%d",ip[0],ip[1],ip[2],ip[3]);
//!                         MSGBUF(ipmask,"%d.%d.%d.%d",ip[4],ip[5],ip[6],ip[7]);
//!
//!                         if (grp->ttipaddr)
//!                             free( grp->ttipaddr );
//!                         grp->ttipaddr = strdup( ipaddr );
//!
//!                         if (grp->ttnetmask)
//!                             free( grp->ttnetmask );
//!                         grp->ttnetmask = strdup( ipmask );
//!
//!                         /* PROGRAMMING NOTE: cannot change the interface
//!                            once it has been enabled. Thus we temporarily
//!                            disable it, make our changes, and then enable
//!                            it again.
//!                         */
//!                         if ((was_enabled = grp->enabled))
//!                             qeth_disable_interface( dev, grp );
//!
//!                         if ((rc = qeth_set_addr_parms( dev, grp )) != 0)
//!                         {
//!                             qeth_errnum_msg( dev, grp, rc,
//!                                 "E", "IPA_CMD_SETIP failed" );
//!                             retcode = IPA_RC_FFFF;
//!                         }
//!
//!                         if (was_enabled)
//!                             qeth_enable_interface( dev, grp );
//!                     }
//! #if defined(ENABLE_IPV6)
//!                     else if (proto == IPA_PROTO_IPV6)
//!                     {
//!                         /* Hmm... What does one do with an IPv6 address? */
//!                         /* SetDestAddr isn't valid for IPv6.             */
//!                     }
//! #endif /*defined(ENABLE_IPV6)*/

                    STORE_HW(ipa->rc,retcode);
                }
                break;

            case IPA_CMD_QIPASSIST:  /* 0xB2 */
                DBGTRC(dev, "QIPASSIST (L3 Query IP Assist)\n");
                grp->ipae |= IPA_SETADAPTERPARMS;
                STORE_HW(ipa->rc,IPA_RC_OK);
                break;

            case IPA_CMD_SETASSPARMS:  /* 0xB3 */
                {
                MPC_IPA_SAS *sas = (MPC_IPA_SAS*)(ipa+1);
                U32 ano;
                U16 cmd;

                    FETCH_FW(ano,sas->hdr.ano);    /* Assist number */
                    FETCH_HW(cmd,sas->hdr.cmd);    /* Command code */
                    DBGTRC(dev, "SETASSPARMS (L3 Set IP Assist parameters: %8.8x, %4.4x)\n",ano,cmd);

                    if (!(ano & grp->ipas)) {
                        STORE_HW(ipa->rc,IPA_RC_NOTSUPP);
                        break;
                    }

                    switch(cmd) {

                    case IPA_SAS_CMD_START:      /* 0x0001 */
                        grp->ipae |= ano;
                        STORE_HW(ipa->rc,IPA_RC_OK);
                        STORE_HW(sas->hdr.rc,IPA_RC_OK);
                        break;

                    case IPA_SAS_CMD_STOP:       /* 0x0002 */
                        grp->ipae &= (0xFFFFFFFF - ano);
                        STORE_HW(ipa->rc,IPA_RC_OK);
                        STORE_HW(sas->hdr.rc,IPA_RC_OK);
                        break;

                    case IPA_SAS_CMD_CONFIGURE:  /* 0x0003 */
                    case IPA_SAS_CMD_ENABLE:     /* 0x0004 */
                    case IPA_SAS_CMD_0005:       /* 0x0005 */
                    case IPA_SAS_CMD_0006:       /* 0x0006 */
                        STORE_HW(ipa->rc,IPA_RC_OK);
                        STORE_HW(sas->hdr.rc,IPA_RC_OK);
                        break;

                    default:
                        DBGTRC(dev, "SETASSPARMS unsupported command\n");
                    /*  STORE_HW(sas->hdr.rc,IPA_RC_UNSUPPORTED_SUBCMD);  */
                        STORE_HW(ipa->rc,IPA_RC_UNSUPPORTED_SUBCMD);
                    }

                }
                /* end case IPA_CMD_SETASSPARMS: */
                break;

            case IPA_CMD_SETIPM:  /* 0xB4 */
                DBGTRC(dev, "L3 Set IPM\n");
                STORE_HW(ipa->rc,IPA_RC_OK);
                break;

            case IPA_CMD_DELIPM:  /* 0xB5 */
                DBGTRC(dev, "L3 Del IPM\n");
                STORE_HW(ipa->rc,IPA_RC_OK);
                break;

            case IPA_CMD_SETRTG:  /* 0xB6 */
                DBGTRC(dev, "L3 Set Routing\n");
                STORE_HW(ipa->rc,IPA_RC_OK);
                break;

            case IPA_CMD_DELIP:  /* 0xB7 */
                DBGTRC(dev, "L3 Del IP\n");
                STORE_HW(ipa->rc,IPA_RC_OK);
                break;

            case IPA_CMD_CREATEADDR:  /* 0xC3 */
                {
                BYTE *sip = (BYTE*)(ipa+1);

                    DBGTRC(dev, "L3 Create IPv6 addr from MAC\n");
                    memcpy( sip+0, &grp->iMAC[0], IFHWADDRLEN/2 );
                    sip[3] = 0xFF;
                    sip[4] = 0xFE;
                    memcpy( sip+5, &grp->iMAC[3], IFHWADDRLEN/2 );
                    STORE_HW(ipa->rc,IPA_RC_OK);
                }
                break;

            case IPA_CMD_SETDIAGASS:  /* 0xB9 */
                DBGTRC(dev, "L3 Set Diag parms\n");
                STORE_HW(ipa->rc,IPA_RC_OK);
                break;

            default:
                DBGTRC(dev, "Invalid IPA Cmd(%02x)\n",ipa->cmd);
                STORE_HW(ipa->rc,IPA_RC_NOTSUPP);
            }
            /* end switch(ipa->cmd) */

//          ipa->iid = IPA_IID_ADAPTER | IPA_IID_REPLY;
            ipa->iid = IPA_IID_HOST;
            grp->ipae &= grp->ipas;
            STORE_FW(ipa->ipas,grp->ipas);
            if (lendata >= SIZE_IPA)
                STORE_FW(ipa->ipae,grp->ipae);

            // Add response buffer to chain.
            add_buffer_to_chain_and_signal_event( grp, rsp_bhr );
        }
        /* end case RRH_TYPE_IPA: */
        break;

    default:
        DBGTRC(dev, "Invalid Type=%2.2x\n",req_rrh->type);
    }
    /* end switch(req_rrh->type) */

}
/* end osa_adapter_cmd */


/*-------------------------------------------------------------------*/
/* Device Command Routine                                            */
/*-------------------------------------------------------------------*/
static void osa_device_cmd(DEVBLK *dev, MPC_IEA *iea, int ieasize)
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;
OSA_BHR *rsp_bhr;
MPC_IEAR *iear;
U16 reqtype;

    /* Allocate a buffer to which the IEA will be copied */
    /* and then modified, to become the IEAR.            */
    rsp_bhr = alloc_buffer( dev, ieasize+10 );
    if (!rsp_bhr)
        return;
    rsp_bhr->datalen = ieasize;

    /* Point to response IEAR. */
    iear = (MPC_IEAR*)((BYTE*)rsp_bhr + SizeBHR);

    /* Copy request to response buffer. */
    memcpy(iear, iea, ieasize);

    FETCH_HW(reqtype, iea->type);

    switch(reqtype) {

    case IDX_ACT_TYPE_READ:
        if((iea->port & IDX_ACT_PORT_MASK) != OSA_PORTNO)
        {
            DBGTRC(dev, "IDX Activate Read: Invalid OSA Port %d for %s Device %4.4x\n",
                (iea->port & IDX_ACT_PORT_MASK),dev->devnum);
            dev->qdio.idxstate = MPC_IDX_STATE_INACTIVE;
        }
        else
        {
            iear->resp &= (0xFF - IDX_RSP_RESP_MASK);
            iear->resp |= IDX_RSP_RESP_OK;
            iear->flags = (IDX_RSP_FLAGS_NOPORTREQ + IDX_RSP_FLAGS_40);
            STORE_FW(iear->token, QTOKEN1);
            STORE_HW(iear->flevel, IDX_RSP_FLEVEL_0201);
            STORE_FW(iear->uclevel, UCLEVEL);

            dev->qdio.idxstate = MPC_IDX_STATE_ACTIVE;
        }
        break;

    case IDX_ACT_TYPE_WRITE:

        memcpy( grp->gtissue, iea->token, MPC_TOKEN_LENGTH );  /* Remember guest token issuer */
        grp->ipas = IPA_SUPP;
        grp->ipae = 0;

        if((iea->port & IDX_ACT_PORT_MASK) != OSA_PORTNO)
        {
            DBGTRC(dev, "IDX Activate Write: Invalid OSA Port %d for device %4.4x\n",
                (iea->port & IDX_ACT_PORT_MASK),dev->devnum);
            dev->qdio.idxstate = MPC_IDX_STATE_INACTIVE;
        }
        else
        {
            iear->resp &= (0xFF - IDX_RSP_RESP_MASK);
            iear->resp |= IDX_RSP_RESP_OK;
            iear->flags = (IDX_RSP_FLAGS_NOPORTREQ + IDX_RSP_FLAGS_40);
            STORE_FW(iear->token, QTOKEN1);
            STORE_HW(iear->flevel, IDX_RSP_FLEVEL_0201);
            STORE_FW(iear->uclevel, UCLEVEL);

            dev->qdio.idxstate = MPC_IDX_STATE_ACTIVE;
        }
        break;

    default:
        DBGTRC(dev, "IDX Activate: Invalid Request %4.4x for device %4.4x\n",
            reqtype,dev->devnum);
        dev->qdio.idxstate = MPC_IDX_STATE_INACTIVE;

        // Free the buffer.
        free( rsp_bhr );
        rsp_bhr = NULL;

        break;
    }

    // Add response buffer to chain.
    add_buffer_to_chain_and_signal_event( grp, rsp_bhr );
}


/*-------------------------------------------------------------------*/
/* Raise Adapter Interrupt                                           */
/*-------------------------------------------------------------------*/
static void raise_adapter_interrupt(DEVBLK *dev)
{
    DBGTRC(dev, "Adapter Interrupt dev(%4.4X)\n",dev->devnum);

    obtain_lock(&dev->lock);
    dev->pciscsw.flag2 |= SCSW2_Q | SCSW2_FC_START;
    dev->pciscsw.flag3 |= SCSW3_SC_INTER | SCSW3_SC_PEND;
    dev->pciscsw.chanstat = CSW_PCI;
    QUEUE_IO_INTERRUPT(&dev->pciioint);
    release_lock (&dev->lock);

    /* Update interrupt status */
    OBTAIN_INTLOCK(devregs(dev));
    UPDATE_IC_IOPENDING();
    RELEASE_INTLOCK(devregs(dev));
}


/*-------------------------------------------------------------------*/
/* Internal function return code flags                               */
/*-------------------------------------------------------------------*/

typedef short QRC;              /* Internal function return code     */

#define QRC_SUCCESS      0      /* Successful completion             */
#define QRC_EIOERR      -1      /* Device i/o error reading/writing  */
#define QRC_ESTORCHK    -2      /* STORCHK failure (Prot Key Chk)    */
#define QRC_ENOSPC      -3      /* Out of Storage Blocks             */
#define QRC_EPKEOF      -4      /* EOF while looking for packets     */
#define QRC_EPKTYP      -5      /* Unsupported output packet type    */
#define QRC_EPKSIZ      -6      /* Output packet/frame too large     */
#define QRC_EZEROBLK    -7      /* Zero Length Storage Block         */
#define QRC_EPKSBLEN    -8      /* Packet length <-> SBALE mismatch  */
#define QRC_ESBPKCPY    -9      /* Packet copy wrong ending SBALE    */
#define QRC_ESBNOEOF   -10      /* No last Last Storage Block flag   */


/*-------------------------------------------------------------------*/
/* Helper function to report errors associated with an SBALE.        */
/*-------------------------------------------------------------------*/
QRC SBALE_Error( char* msg, QRC qrc, DEVBLK* dev,
                 QDIO_SBAL *sbal, BYTE sbalk, int sb )
{
    char errmsg[256] = {0};
    U64 sbala = (U64)((BYTE*)sbal - dev->mainstor);
    U64 sba;
    U32 sblen;

    FETCH_DW( sba,   sbal->sbale[sb].addr   );
    FETCH_FW( sblen, sbal->sbale[sb].length );

    MSGBUF( errmsg, msg, sb, sbala, sbalk, sba, sblen,
        sbal->sbale[sb].flags[0],
        sbal->sbale[sb].flags[3]);

    // HHC03985 "%1d:%04X %s: %s"
    WRMSG( HHC03985, "E", SSID_TO_LCSS(dev->ssid), dev->devnum,
        "QDIO", errmsg );

    return qrc;
}
/*-------------------------------------------------------------------*/
/* Helper macro to call above helper function.                       */
/*-------------------------------------------------------------------*/
#define SBALE_ERROR(_qrc,_dev,_sbal,_sbalk,_sb)                     \
    SBALE_Error( "** " #_qrc " **: SBAL(%d) @ %llx [%02X]:"         \
        " Addr: %llx Len: %d flags[0,3]: %2.2X %2.2X\n",            \
        (_qrc), (_dev), (_sbal), (_sbalk), (_sb))


/*-------------------------------------------------------------------*/
/* Helper macro to check for logically last SBALE                    */
/*-------------------------------------------------------------------*/
#define WR_LOGICALLY_LAST_SBALE( _flag0 )   (!grp->wrpack ?         \
  ( (_flag0) & SBALE_FLAG0_LAST_ENTRY) :                            \
  (((_flag0) & SBALE_FLAG0_LAST_ENTRY) ||                           \
  (((_flag0) & SBALE_FLAG0_FRAG_LAST) == SBALE_FLAG0_FRAG_LAST )))


/*-------------------------------------------------------------------*/
/* Helper macro to check if absolutely the last SBALE                */
/*-------------------------------------------------------------------*/
#define IS_ABSOLUTELY_LAST_SBALE( _flag0 )                          \
  ((_flag0) & SBALE_FLAG0_LAST_ENTRY)


/*-------------------------------------------------------------------*/
/* Helper macro to set the SBALE fragment flags                      */
/*-------------------------------------------------------------------*/
#define SET_SBALE_FRAG( _flag0, _frag )                             \
  do {                                                              \
    (_flag0) &= ~(SBALE_FLAG0_LAST_ENTRY | SBALE_FLAG0_FRAG_MASK);  \
    (_flag0) |= (_frag);                                            \
  } while (0)


/*-------------------------------------------------------------------*/
/* Determine Layer 3 IPv4 cast type                                  */
/*-------------------------------------------------------------------*/
static inline int l3_cast_type_ipv4( U32 dstaddr, OSA_GRP *grp )
{
    if (!dstaddr)
        return L3_CAST_NOCAST;
    if ((dstaddr & 0xE0000000) == 0xE0000000)
        return L3_CAST_MULTICAST;
    if ((dstaddr & grp->pfxmask4) == grp->pfxmask4)
        return L3_CAST_BROADCAST;
    return L3_CAST_UNICAST;
}


/*-------------------------------------------------------------------*/
/* Determine Layer 3 IPv6 cast type                                  */
/*-------------------------------------------------------------------*/
static inline int l3_cast_type_ipv6( BYTE* dest_addr, OSA_GRP *grp )
{
    static const BYTE dest_zero[16] = {0};
    BYTE dest_work[16];
    int i;

    if (dest_addr[0] == 0xFF)
        return L3_CAST_MULTICAST;

    if (memcmp( dest_addr, dest_zero, 16 ) == 0)
        return L3_CAST_NOCAST;

    memcpy( dest_work, dest_addr, 16 );

    /* Ignore prefix bits */
    for (i=0; i < 16 && grp->pfxmask6[i] != 0xFF; i++)
        dest_work[i] &= grp->pfxmask6[i];

    /* If non-prefix bits are all zero then anycast */
    if (memcmp( dest_work, dest_zero, 16 ) == 0)
        return L3_CAST_ANYCAST;

    return L3_CAST_UNICAST;
}


/*-------------------------------------------------------------------*/
/* Determine if TUN/TAP device has more packets waiting for us.      */
/* Does a 'select' on the TUN/TAP device using a zero timeout        */
/* and returns 1 if more packets are waiting or 0 (false) otherwise. */
/* Note: boolean function. Does not report errors. If the select     */
/* call fails then this function simply returns 0 = false (EOF).     */
/*-------------------------------------------------------------------*/
static BYTE more_packets( DEVBLK* dev )
{
    fd_set readset;
    struct timeval tv = {0,0};
    FD_ZERO( &readset );
    FD_SET( dev->fd, &readset );
    return (select( dev->fd+1, &readset, NULL, NULL, &tv ) > 0);
}


/*-------------------------------------------------------------------*/
/* Read one packet/frame from TUN/TAP device into dev->buf.          */
/* dev->buflen updated with length of packet/frame just read.        */
/*-------------------------------------------------------------------*/
static QRC read_packet( DEVBLK* dev, OSA_GRP *grp )
{
    int errnum;
    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "b4 tt read", 0, dev->bufsize, 0 );
    dev->buflen = TUNTAP_Read( dev->fd, dev->buf, dev->bufsize );
    errnum = errno;
    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "af tt read", 0, dev->bufsize, dev->buflen );

    if (unlikely(dev->buflen < 0))
    {
        if (errnum == EAGAIN)
        {
            errno = EAGAIN;
            return QRC_EPKEOF;
        }
        else
        {
            // HHC03972 "%1d:%04X %s: error reading from device %s: %d %s"
            WRMSG(HHC03972, "E", SSID_TO_LCSS(dev->ssid), dev->devnum,
                "QETH", grp->ttifname, errnum, strerror( errnum ));
            errno = errnum;
            return QRC_EIOERR;
        }
    }

    if (unlikely(dev->buflen == 0))
    {
        errno = EAGAIN;
        return QRC_EPKEOF;
    }

    return QRC_SUCCESS;
}


/*-------------------------------------------------------------------*/
/* Write one L2/L3 packet/frame to the TUN/TAP device.               */
/*-------------------------------------------------------------------*/
static QRC write_packet( DEVBLK* dev, OSA_GRP *grp,
                         BYTE* pkt, int pktlen )
{
    int wrote, errnum;

    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "b4 tt write", 0, pktlen, 0 );
    wrote = TUNTAP_Write( dev->fd, pkt, pktlen );
    errnum = errno;
    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "af tt write", 0, pktlen, 0 );

    if (likely(wrote == pktlen))
    {
        dev->qdio.txcnt++;
        return QRC_SUCCESS;
    }

    // HHC03971 "%1d:%04X %s: error writing to device %s: %d %s"
    WRMSG(HHC03971, "E", SSID_TO_LCSS(dev->ssid), dev->devnum,
        "QETH", grp->ttifname, errnum, strerror( errnum ));
    errno = errnum;
    return QRC_EIOERR;
}


/*-------------------------------------------------------------------*/
/* Copy data fragment into OSA queue buffer storage.                 */
/* Uses the entries from the passed Storage Block Address List to    */
/* split the fragment across several Storage Blocks as needed.       */
/*-------------------------------------------------------------------*/
/* src points to the data to be copied into the Storage Block.       */
/* rem is the src data length (how much remains to be copied).       */
/* sbal points to the Storage Block Address List for the buffer.     */
/* sbalk is the Storage Block's associated protection key.           */
/* sb is a ptr to the current SBAL Storage Block number.             */
/* frag0 is the SBALE's current first/middle fragment flag.          */
/* sboff is a ptr to the offset into the current Storage Block.      */
/* sbrem is a ptr to how much of the current Storage Block remains.  */
/*-------------------------------------------------------------------*/
static QRC copy_fragment_to_storage( DEVBLK* dev, QDIO_SBAL *sbal,
                                     BYTE sbalk, int* sb, BYTE* frag0,
                                     U32* sboff, U32* sbrem,
                                     BYTE* src, int rem )
{
    U64 sba;                            /* Storage Block Address     */
    BYTE *dst = NULL;                   /* Destination address       */
    int len;                            /* Copy length (work)        */

    /* While src bytes remain to be copied */
    while (rem > 0)
    {
        /* End of current Storage Block? */
        if (!*sbrem && *sboff)
        {
            /* Done using this Storage Block */
            STORE_FW( sbal->sbale[*sb].length, *sboff );
            STORE_FW( sbal->sbale[*sb].flags,     0   );
            SET_SBALE_FRAG( sbal->sbale[*sb].flags[0], *frag0 );

            /* Go on to next Storage Block */
            if (*sb >= (QMAXSTBK-1))
                return SBALE_ERROR( QRC_ENOSPC, dev,sbal,sbalk,*sb);
            *sb = *sb + 1;
            *frag0 = SBALE_FLAG0_FRAG_MIDDLE;
            *sboff = 0;
        }

        /* Starting a new Storage Block? */
        if (!*sboff || !dst)
        {
            /* Check the Storage Block's length and key */
            FETCH_DW(  sba,   sbal->sbale[*sb].addr   );
            FETCH_FW( *sbrem, sbal->sbale[*sb].length );
            if (!*sbrem)
                return SBALE_ERROR( QRC_EZEROBLK, dev,sbal,sbalk,*sb);
            if (STORCHK(sba,(*sbrem)-1,sbalk,STORKEY_CHANGE,dev))
                return SBALE_ERROR( QRC_ESTORCHK, dev,sbal,sbalk,*sb);
            *sbrem -= *sboff;

            /* Calculate new destination address */
            dst = (BYTE*)(dev->mainstor + sba + *sboff);
        }

        /* Continue copying data to Storage Block */
        len = min( *sbrem, (U32)rem );
        memcpy( dst, src, len );

        dst   += len;
        src   += len;
        rem   -= len;
        *sboff += len;
        *sbrem -= len;
    }

    /* PROGRAMMING NOTE: the CALLER will mark last fragment! */
    return QRC_SUCCESS;
}


/*-------------------------------------------------------------------*/
/* Copy packet/frame data from one/more OSA queue storage buffers    */
/* into DEVBLK device buffer. Uses the entries from the passed       */
/* Storage Block Address List to copy the packet/frame data which    */
/* might be split across several Storage Blocks. Stops when either   */
/* the entire output packet has been consolidated into the DEVBLK    */
/* buffer or the ending Storage Block is reached and consumed.       */
/*-------------------------------------------------------------------*/
/* sbal points to the Storage Block Address List for the buffer.     */
/* sbalk is the associated protection key for the queue buffer.      */
/* sb is a ptr to the current SBAL Storage Block number.             */
/* sbsrc is where in the first Storage Block to begin copying from.  */
/* sblen is the length of the first Storage Block minus the OSA hdr. */
/* dev->bufres holds the expected packet/frame size and dev->buflen  */
/* starts at zero. Both are updated as the copying proceeds.         */
/*-------------------------------------------------------------------*/
static QRC copy_storage_fragments( DEVBLK* dev, OSA_GRP *grp,
                                   QDIO_SBAL *sbal, BYTE sbalk,
                                   int* sb, BYTE* sbsrc, U32 sblen )
{
    U64 sba;                            /* Storage Block Address     */
    BYTE *dst;                          /* Destination address       */
    U32 len;                            /* Copy length               */

    dst = dev->buf + dev->buflen;       /* Build destination pointer */

    /* Copy data from each Storage Block in turn until the entire
       packet/frame has been copied or we reach the ending Block. */
    while (dev->bufres > 0)
    {
        /* End of current storage block? */
        if (!sblen)
        {
            /* Is this the last storage block? */
            if (WR_LOGICALLY_LAST_SBALE( sbal->sbale[*sb].flags[0] ))
            {
                /* We have copied as much data as we possibly can but
                dev->bufres is not zero so the Storage Blocks Entries
                are wrong. THIS SHOULD NEVER OCCUR since our device
                buffer size is always set to the maximum size of 64K. */
                return SBALE_ERROR( QRC_EPKSBLEN, dev,sbal,sbalk,*sb);
            }

            /* Request interrupt if needed */
            if (sbal->sbale[*sb].flags[3] & SBALE_FLAG3_PCI_REQ)
            {
                SET_DSCI(dev,DSCI_IOCOMP);
                grp->reqpci = TRUE;
            }

            /* Retrieve the next storage block entry */
            if (*sb >= (QMAXSTBK-1))
                return SBALE_ERROR( QRC_ENOSPC, dev,sbal,sbalk,*sb);
            *sb = *sb + 1;
            FETCH_DW( sba,   sbal->sbale[*sb].addr   );
            FETCH_FW( sblen, sbal->sbale[*sb].length );
            if (!sblen)
                return SBALE_ERROR( QRC_EZEROBLK, dev,sbal,sbalk,*sb);
            if (STORCHK( sba, sblen-1, sbalk, STORKEY_CHANGE, dev))
                return SBALE_ERROR( QRC_ESTORCHK, dev,sbal,sbalk,*sb);

            /* Point to new data source */
            sbsrc = (BYTE*)(dev->mainstor + sba);
        }

        /* Copying packet/frame to device from this storage block */
        len = min( (U32)dev->bufres, sblen );
        memcpy( dst, sbsrc, len );

        dst         += len;
        dev->buflen += len;
        dev->bufres -= len;
        sbsrc       += len;
        sblen       -= len;
    }

    return QRC_SUCCESS;
}


/*-------------------------------------------------------------------*/
/* Copy packet/frame from dev->buf into OSA queue buffer storage.    */
/* Uses the entries from the passed Storage Block Address List to    */
/* split the packet/frame across several Storage Blocks as needed.   */
/* dev->buflen should be set to the length of the packet/frame.      */
/*-------------------------------------------------------------------*/
/* sbal points to the Storage Block Address List for the buffer.     */
/* sb is the Storage Block number to begin processing with.          */
/* sbalk is the associated protection key for the queue buffer.      */
/* hdr points to pre-built OSA_HDR2/OSA_HDR3 and hdrlen is its size. */
/*-------------------------------------------------------------------*/
static QRC copy_packet_to_storage( DEVBLK* dev, OSA_GRP *grp,
                                   QDIO_SBAL *sbal, int sb, BYTE sbalk,
                                   BYTE* hdr, int hdrlen )
{
    int ssb = sb;                       /* Starting Storage Block    */
    U32 sboff = 0;                      /* Storage Block offset      */
    U32 sbrem = 0;                      /* Storage Block remaining   */
    BYTE frag0;                         /* SBALE fragment flag       */
    QRC qrc;                            /* Internal return code      */

    /* Start with the header first */
    frag0 = SBALE_FLAG0_FRAG_FIRST;
    if ((qrc = copy_fragment_to_storage( dev, sbal, sbalk,
        &sb, &frag0, &sboff, &sbrem, hdr, hdrlen )) < 0 )
        return qrc;

    /* Then copy the packet/frame */
    if ((qrc = copy_fragment_to_storage( dev, sbal, sbalk,
        &sb, &frag0, &sboff, &sbrem, dev->buf, dev->buflen )) < 0 )
        return qrc;

    /* Mark last fragment */
    frag0 = SBALE_FLAG0_FRAG_LAST;
    STORE_FW( sbal->sbale[sb].length, sboff );
    STORE_FW( sbal->sbale[sb].flags,     0   );
    SET_SBALE_FRAG( sbal->sbale[sb].flags[0], frag0 );

    /* Count packets received */
    dev->qdio.rxcnt++;

    /* Dump the SBALE's we consumed */
    if (grp->debug)
    {
        int  i;
        for (i=ssb; i <= sb; i++)
        {
            FETCH_FW( sbrem, sbal->sbale[i].length );
            frag0 = sbal->sbale[i].flags[0];
            DBGTRC( dev, "Input SBALE(%d): flag: %02X Len: %04X (%d)\n",
                i, frag0, sbrem, sbrem );
        }
    }

    return QRC_SUCCESS;
}


/*-------------------------------------------------------------------*/
/* Read one L2 frame from TAP device into queue buffer storage.      */
/*-------------------------------------------------------------------*/
static QRC read_L2_packets( DEVBLK* dev, OSA_GRP *grp,
                            QDIO_SBAL *sbal, BYTE sbalk )
{
    OSA_HDR2 o2hdr;
    ETHFRM* eth;
    int mactype;
    QRC qrc;
    int sb = 0;     /* Start with Storage Block zero */

    do {
        /* Find (another) frame for our MAC */
        eth = (ETHFRM*)dev->buf;
        for(;;)
        {
            if ((qrc = read_packet( dev, grp )) < 0)
                return qrc; /*(probably EOF)*/

            /* Verify the frame is being sent to us */
            if (!(mactype = validate_mac( eth->bDestMAC, MAC_TYPE_ANY, grp )))
                continue; /* (try next packet) */
            /* We found a frame being sent to our MAC */
            break;
        }

        /* Build the Layer 2 OSA header */
        memset( &o2hdr, 0, sizeof( OSA_HDR2 ));
        STORE_HW( o2hdr.pktlen, dev->buflen );
        o2hdr.id = HDR_ID_LAYER2;

        switch( mactype & MAC_TYPE_ANY ) {
        case MAC_TYPE_UNICST:
            o2hdr.flags[2] |= HDR2_FLAGS2_UNICAST;
            break;
        case MAC_TYPE_BRDCST:
            o2hdr.flags[2] |= HDR2_FLAGS2_BROADCAST;
            break;
        case MAC_TYPE_MLTCST:
            o2hdr.flags[2] |= HDR2_FLAGS2_MULTICAST;
            break;
        }

        /* Dump the frame just received */
        if( grp->debug )
        {
            MPC_DUMP_DATA( "INPUT L2 HDR", (BYTE*)&o2hdr,   (int)sizeof(o2hdr), '<' );
            MPC_DUMP_DATA( "INPUT L2 FRM", (BYTE*)dev->buf, (int)dev->buflen,   '<' );
        }

        /* Copy header and frame to buffer storage block(s) */
        qrc = copy_packet_to_storage( dev, grp, sbal, sb, sbalk,
                                      (BYTE*) &o2hdr, sizeof( o2hdr ));
    }
    while (qrc >= 0 && grp->rdpack && more_packets( dev ) && ++sb < QMAXSTBK);

    /* Mark end of buffer */
    if (sb >= QMAXSTBK) sb--;
    sbal->sbale[sb].flags[0] |= SBALE_FLAG0_LAST_ENTRY;

    return qrc;
}


/*-------------------------------------------------------------------*/
/* Read one L3 packet from TUN device into queue buffer storage.     */
/*-------------------------------------------------------------------*/
static QRC read_L3_packets( DEVBLK* dev, OSA_GRP *grp,
                            QDIO_SBAL *sbal, BYTE sbalk )
{
    static const BYTE udp = 17;
    QRC qrc;
    OSA_HDR3 o3hdr;
    int sb = 0;     /* Start with Storage Block zero */

    do
    {
        /* Read another packet into the device buffer */
        if ((qrc = read_packet( dev, grp )) != 0)
            return qrc;

        /* Build the Layer 3 OSA header */
        memset( &o3hdr, 0, sizeof( OSA_HDR3 ));
        STORE_HW( o3hdr.length, dev->buflen );
        o3hdr.id = HDR_ID_LAYER3;
//      STORE_HW( o3hdr.frame_offset, ???? ); // TSO only?
//      STORE_FW( o3hdr.token, ???? );

        if (grp->ttipaddr6)
        {
            IP6FRM* ip6 = (IP6FRM*)dev->buf;
            memcpy( o3hdr.dest_addr, ip6->bDstAddr, 16 );
            o3hdr.flags |= HDR3_FLAGS_PASSTHRU | HDR3_FLAGS_IPV6 |
                (l3_cast_type_ipv6( o3hdr.dest_addr, grp ) & HDR3_FLAGS_CASTMASK);
            o3hdr.ext_flags = (ip6->bNextHeader == udp) ? HDR3_EXFLAG_UDP : 0;
        }
        else
        {
            U32 dstaddr;
            U16 checksum;
            IP4FRM* ip4 = (IP4FRM*)dev->buf;
            FETCH_FW( dstaddr, &ip4->lDstIP );
            STORE_FW( &o3hdr.dest_addr[12], dstaddr );
            FETCH_HW( checksum, ip4->hwChecksum );
            STORE_HW( o3hdr.in_cksum, checksum );
            o3hdr.flags = l3_cast_type_ipv4( dstaddr, grp );
            o3hdr.ext_flags = (ip4->bProtocol == udp) ? HDR3_EXFLAG_UDP : 0;
        }

        /* Dump the packet just received */
        if( grp->debug )
        {
            MPC_DUMP_DATA( "INPUT L3 HDR", (BYTE*)&o3hdr,   (int)sizeof(o3hdr), '<' );
            MPC_DUMP_DATA( "INPUT L3 PKT", (BYTE*)dev->buf, (int)dev->buflen,   '<' );
        }

        /* Copy header and packet to buffer storage block(s) */
        qrc = copy_packet_to_storage( dev, grp, sbal, sb, sbalk,
                                      (BYTE*) &o3hdr, sizeof( o3hdr ));
    }
    while (qrc >= 0 && grp->rdpack && more_packets( dev ) && ++sb < QMAXSTBK);

    /* Mark end of buffer */
    if (sb >= QMAXSTBK) sb--;
    sbal->sbale[sb].flags[0] |= SBALE_FLAG0_LAST_ENTRY;

    return qrc;
}


/*-------------------------------------------------------------------*/
/* Write all packets/frames in this primed buffer. Automatically     */
/* handles output packing mode by continuing to next storage Block.  */
/* sbal points to the Storage Block Address List for the buffer.     */
/* sbalk is the associated protection key for the queue buffer.      */
/*-------------------------------------------------------------------*/
static QRC write_buffered_packets( DEVBLK* dev, OSA_GRP *grp,
                                   QDIO_SBAL *sbal, BYTE sbalk )
{
    U64 sba;                            /* Storage Block Address     */
    BYTE* hdr;                          /* Ptr to OSA packet header  */
    BYTE* pkt;                          /* Ptr to packet or frame    */
    U32 sblen;                          /* Length of Storage Block   */
    int hdrlen;                         /* Length of OSA header      */
    int pktlen;                         /* Packet or frame length    */
    int sb;                             /* Storage Block number      */
    int ssb;                            /* Starting Storage Block    */
    QRC qrc;                            /* Internal return code      */
    BYTE hdr_id;                        /* OSA Header Block Id       */
    BYTE flag0;                         /* Storage Block Flag        */

    sb = 0;                             /* Start w/Storage Block 0   */

    do
    {
        /* Save starting Storage Block number */
        ssb = sb;

        /* Retrieve the (next) Storage Block and check its key */
        FETCH_DW( sba,   sbal->sbale[sb].addr   );
        FETCH_FW( sblen, sbal->sbale[sb].length );
        if (!sblen)
            return SBALE_ERROR( QRC_EZEROBLK, dev,sbal,sbalk,sb);
        if (STORCHK( sba, sblen-1, sbalk, STORKEY_REF, dev ))
            return SBALE_ERROR( QRC_ESTORCHK, dev,sbal,sbalk,sb);

        /* Get pointer to OSA header */
        hdr = (BYTE*)(dev->mainstor + sba);

        /* Verify Block is long enough to hold the full OSA header.
           FIX ME: there is nothing in the specs that requires the
           header to not span multiple Storage Blocks so we should
           should probably support it, but at the moment we do not. */
        if (sblen < max(sizeof(OSA_HDR2),sizeof(OSA_HDR3)))
            WRMSG( HHC03983, "W", SSID_TO_LCSS(dev->ssid), dev->devnum,
                "QETH", "** FIX ME ** OSA_HDR spans multiple storage blocks." );

        /* Determine if Layer 2 Ethernet frame or Layer 3 IP packet */
        hdr_id = hdr[0];
        switch (hdr_id)
        {
        U16 length;
        case HDR_ID_LAYER2:
        {
            ETHFRM* eth;
            OSA_HDR2* o2hdr = (OSA_HDR2*)hdr;
            hdrlen = sizeof(OSA_HDR2);
            pkt = hdr + hdrlen;
            FETCH_HW( length, o2hdr->pktlen );
            pktlen = length;
            eth = (ETHFRM*)pkt;
            break;
        }
        case HDR_ID_LAYER3:
        {
            OSA_HDR3* o3hdr = (OSA_HDR3*)hdr;
            hdrlen = sizeof(OSA_HDR3);
            pkt = hdr + hdrlen;
            FETCH_HW( length, o3hdr->length );
            pktlen = length;
            break;
        }
        case HDR_ID_TSO:
        case HDR_ID_OSN:
        default:
            return SBALE_ERROR( QRC_EPKTYP, dev,sbal,sbalk,sb);
        }

        /* Make sure the packet/frame fits in the device buffer */
        if (pktlen > dev->bufsize)
            return SBALE_ERROR( QRC_EPKSIZ, dev,sbal,sbalk,sb);

        /* Copy the actual packet/frame into the device buffer */
        sblen -= hdrlen;
        dev->bufres = pktlen;
        dev->buflen = 0;

        if ((qrc = copy_storage_fragments( dev, grp, sbal, sbalk,
                                           &sb, pkt, sblen )) < 0)
            return qrc;

        /* Save ending flag */
        flag0 = sbal->sbale[sb].flags[0];

        /* Trace the pack/frame if debugging is enabled */
        if (grp->debug)
        {
            DBGTRC( dev, "Output SBALE(%d-%d): Len: %04X (%d)\n",
                ssb, sb, dev->buflen, dev->buflen );
            MPC_DUMP_DATA( "OUTPUT BUF", dev->buf, dev->buflen, '>' );
        }

        qrc = write_packet( dev, grp, dev->buf, dev->buflen );
    }
    while (qrc >= 0 && !IS_ABSOLUTELY_LAST_SBALE( flag0 ) && ++sb < QMAXSTBK);

    if (sb < QMAXSTBK)
        return qrc;

    return SBALE_ERROR( QRC_ESBNOEOF, dev,sbal,sbalk,sb-1);
}


/*-------------------------------------------------------------------*/
/*                   Process Input Queues                            */
/*-------------------------------------------------------------------*/
/* We must go through the queues/buffers in a round robin manner     */
/* so that buffers are re-used on a LRU (Least Recently Used) basis. */
/* When no buffers are available we must keep our current position.  */
/* When a buffer becomes available we will advance to that location. */
/* When we reach the end of the buffer queue we will advance to the  */
/* next available queue. When a queue is newly enabled we start at   */
/* the beginning of the queue (this is handled in signal adapter).   */
/*-------------------------------------------------------------------*/
static void process_input_queues( DEVBLK *dev )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;
int sqn = dev->qdio.i_qpos;             /* Starting queue number     */
int mq = dev->qdio.i_qcnt;              /* Maximum number of queues  */
int qn = sqn;                           /* Working queue number      */
int did_read = 0;                       /* Indicates some data read  */

    do
    {
        if(dev->qdio.i_qmask & (0x80000000 >> qn))
        {
        QDIO_SLSB *slsb = (QDIO_SLSB*)(dev->mainstor + dev->qdio.i_slsbla[qn]);
        int sbn = dev->qdio.i_bpos[qn]; /* Starting buffer number    */
        int mb = QMAXBUFS;              /* Maximum number of buffers */
        int bn = sbn;                   /* Working buffer number     */
        QRC qrc;                        /* Internal return code      */

            do
            {
                if(slsb->slsbe[bn] == SLSBE_INPUT_EMPTY)
                {
                QDIO_SL *sl = (QDIO_SL*)(dev->mainstor + dev->qdio.i_sla[qn]);
                U64 sbala;              /* Storage Block Address List*/
                BYTE sk;                /* Storage Key               */

                    DBGTRC(dev, "Input Queue(%d) Buffer(%d)\n", qn, bn);

                    sk = dev->qdio.i_slk[qn];
                    FETCH_DW( sbala, sl->sbala[bn] );

                    /* Verify Storage Block Address List is accessible */
                    if(STORCHK( sbala, sizeof(QDIO_SBAL)-1, sk, STORKEY_REF, dev ))
                    {
                        DBGTRC(dev, "STORCHK Error SBALA(%llx), Key(%2.2X)\n", sbala, sk);
                        qrc = QRC_ESTORCHK;
                    }
                    else
                    {
                        /* Read packets into this empty buffer */
                        QDIO_SBAL *sbal = (QDIO_SBAL*)(dev->mainstor + sbala);
                        sk = dev->qdio.i_sbalk[qn];
                        did_read = 1;

                        if (grp->l3)
                            qrc = read_L3_packets( dev, grp, sbal, sk );
                        else
                            qrc = read_L2_packets( dev, grp, sbal, sk );

                        /* Mark the buffer as having been completed */
                        if (qrc >= 0)
                        {
                            slsb->slsbe[bn] = SLSBE_INPUT_COMPLETED;
                            STORAGE_KEY(dev->qdio.i_slsbla[qn], dev) |= (STORKEY_REF|STORKEY_CHANGE);
                            SET_DSCI(dev,DSCI_IOCOMP);
                            grp->reqpci = TRUE;
                            return;
                        }
                        else if (qrc == QRC_EPKEOF)
                        {
                            /* We didn't find any packets/frames meant
                            for us (perhaps the destination MAC or IP
                            addresses didn't match) so just return. */
                            return;   /* (nothing for us to do) */
                        }
                    }

                    /* Handle errors here since both read_L2_packets
                       and read_L3_packets may also return an error */
                    if (qrc < 0)
                    {
                        slsb->slsbe[bn] = SLSBE_ERROR;
                        STORAGE_KEY(dev->qdio.i_slsbla[qn], dev) |= (STORKEY_REF|STORKEY_CHANGE);
                        SET_ALSI(dev,ALSI_ERROR);
                        grp->reqpci = TRUE;
                        return;
                    }

                } /* end if (SLSBE_INPUT_EMPTY) */

                /* Go on to the next buffer... */
                if(++bn >= mb)
                    bn = 0;
            }
            while ((dev->qdio.i_bpos[qn] = bn) != sbn);

        } /* end if(dev->qdio.i_qmask & (0x80000000 >> qn)) */

        /* Go on to the next queue... */
        if(++qn >= mq)
            qn = 0;
    }
    while ((dev->qdio.i_qpos = qn) != sqn);

    /* PROGRAMMING NOTE: If we did not actually perform a read
       from the tuntap device in the logic above, then we need
       to do so here at this time. We were called for a reason.
       There are supposedly packets to be read but since we did
       not attempt to read any of them we need to do so at this
       time. Failure to do this causes ACTIVATE QUEUES to call
       us continuously, over and over and over again and again
       and again, because its 'select' function still indicates
       that the socket still has unread data waiting to be read.
    */
    if (!did_read && more_packets( dev ))
    {
    char buff[4096];
    int n;
        DBGTRC(dev, "Input dropped (No available buffers)\n");
        PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "b4 tt read2", -1, sizeof(buff), 0 );
        n = TUNTAP_Read( grp->ttfd, buff, sizeof(buff) );
        PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "af tt read2", -1, sizeof(buff), n );
        if(n > 0)
            grp->reqpci = TRUE;
    }
}
/* end process_input_queues */


/*-------------------------------------------------------------------*/
/*                  Process Output Queues                            */
/*-------------------------------------------------------------------*/
static void process_output_queues( DEVBLK *dev )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;
int sqn = dev->qdio.o_qpos;             /* Starting queue number     */
int mq = dev->qdio.o_qcnt;              /* Maximum number of queues  */
int qn = sqn;                           /* Working queue number      */
int found_buff = 0;                     /* Found primed o/p buffer   */

    do
    {
        if(dev->qdio.o_qmask & (0x80000000 >> qn))
        {
        QDIO_SLSB *slsb = (QDIO_SLSB*)(dev->mainstor + dev->qdio.o_slsbla[qn]);
        int sbn = dev->qdio.o_bpos[qn]; /* Starting buffer number    */
        int mb = QMAXBUFS;              /* Maximum number of buffers */
        int bn = sbn;                   /* Working buffer number     */

            do
            {
                if(slsb->slsbe[bn] == SLSBE_OUTPUT_PRIMED)
                {
                QDIO_SL *sl = (QDIO_SL*)(dev->mainstor + dev->qdio.o_sla[qn]);
                U64 sbala;              /* Storage Block Address List*/
                BYTE sk;                /* Storage Key               */
                QRC qrc;                /* Internal return code      */

                    DBGTRC(dev, "Output Queue(%d) Buffer(%d)\n", qn, bn);

                    found_buff = 1;
                    sk = dev->qdio.o_slk[qn];
                    FETCH_DW( sbala, sl->sbala[bn] );

                    /* Verify Storage Block Address List is accessible */
                    if(STORCHK( sbala, sizeof(QDIO_SBAL)-1, sk, STORKEY_REF, dev ))
                    {
                        DBGTRC(dev, "STORCHK Error SBALA(%llx), Key(%2.2X)\n", sbala, sk);
                        qrc = QRC_ESTORCHK;
                    }
                    else
                    {
                        /* Write packets from this primed buffer */
                        QDIO_SBAL *sbal = (QDIO_SBAL*)(dev->mainstor + sbala);

                        sk = dev->qdio.o_sbalk[qn];

                        if ((qrc = write_buffered_packets( dev, grp, sbal, sk )) >= 0)
                            slsb->slsbe[bn] = SLSBE_OUTPUT_COMPLETED;
                    }

                    /* Packets written or an error has ocurred */
                    STORAGE_KEY(dev->qdio.o_slsbla[qn], dev) |= (STORKEY_REF|STORKEY_CHANGE);

                    /* Handle errors */
                    if (qrc < 0)
                    {
                        slsb->slsbe[bn] = SLSBE_ERROR;
                        SET_ALSI(dev,ALSI_ERROR);
                        grp->reqpci = TRUE;
                        return;
                    }

                } /* end if(SLSBE_OUTPUT_PRIMED) */

                /* Go on to the next buffer... */
                if(++bn >= mb)
                    bn = 0;
            }
            while ((dev->qdio.o_bpos[qn] = bn) != sbn);

        } /* end if(dev->qdio.o_qmask & (0x80000000 >> qn)) */

        /* Go on to the next queue... */
        if(++qn >= mq)
            qn = 0;
    }
    while ((dev->qdio.o_qpos = qn) != sqn);
}
/* end process_output_queues */


/*-------------------------------------------------------------------*/
/* Halt device related functions...                                  */
/*-------------------------------------------------------------------*/
static void qeth_signal_halt (OSA_GRP *grp)
{
fd_set readset;
BYTE sig = QDSIG_HALT;

    /* Send signal */
    write_pipe( grp->ppfd[1], &sig, 1 );

    /* Wait for reply */
    FD_ZERO( &readset );
    FD_SET( grp->ppfd[0], &readset );
    select( grp->ppfd[0]+1, &readset, NULL, NULL, NULL );
    read_pipe( grp->ppfd[0], &sig, 1 );
}

static void qeth_halt_device (DEVBLK *dev)
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

    /* Signal ACTIVATE QUEUES loop to exit if QDIO is active */
    if(dev->scsw.flag2 & SCSW2_Q)
    {
        dev->scsw.flag2 &= ~SCSW2_Q;
        qeth_signal_halt(grp);
    }
    else
        if(dev->group->acount == OSA_GROUP_SIZE)
        {
            /* Tell READ loop to not wait for IDX response */
            dev->qdio.idxstate = MPC_IDX_STATE_INACTIVE;
            signal_condition(&grp->qcond);
        }
}


/*-------------------------------------------------------------------*/
/* Initialize the device handler                                     */
/*-------------------------------------------------------------------*/
static int qeth_init_handler ( DEVBLK *dev, int argc, char *argv[] )
{
OSA_GRP *grp;
int grouped = 0;
int i;

    if(!dev->group)
    {
        dev->numsense = 32;
        memset (dev->sense, 0, sizeof(dev->sense));
        dev->numdevid = sizeof(sense_id_bytes);
        memcpy(dev->devid, sense_id_bytes, sizeof(sense_id_bytes));
        dev->devtype = dev->devid[1] << 8 | dev->devid[2];
        dev->chptype[0] = CHP_TYPE_OSD;
        dev->pmcw.flag4 |= PMCW4_Q;

        if(!(grouped = group_device(dev,OSA_GROUP_SIZE)) && !dev->member)
        {
            dev->group->grp_data = grp = malloc(sizeof(OSA_GRP));
            memset (grp, 0, sizeof(OSA_GRP));

            register_mac((BYTE*)"\xFF\xFF\xFF\xFF\xFF\xFF",MAC_TYPE_BRDCST,grp);

            initialize_condition(&grp->qcond);
            initialize_lock(&grp->qlock);
            initialize_lock(&grp->qblock);

            /* Creat ACTIVATE QUEUES signalling pipe */
            create_pipe(grp->ppfd);

            /* Set Non-Blocking mode */
            socket_set_blocking_mode(grp->ppfd[0],0);

            /* Set defaults */
#if defined( OPTION_W32_CTCI )
            grp->tuntap = strdup( tt32_get_default_iface() );
#else /*!defined( OPTION_W32_CTCI )*/
            grp->tuntap = strdup(TUNTAP_NAME);
#endif /*defined( OPTION_W32_CTCI )*/
            grp->ttfd = -1;
        }
        else
            grp = dev->group->grp_data;
    }
    else
        grp = dev->group->grp_data;

    // process all command line options here
    for(i = 0; i < argc; i++)
    {
        if(!strcasecmp("iface",argv[i]) && (i+1) < argc)
        {
            if(grp->tuntap)
                free(grp->tuntap);
            grp->tuntap = strdup(argv[++i]);
            continue;
        }
        else if(!strcasecmp("ifname",argv[i]) && (i+1) < argc)
        {
            strlcpy( grp->ttifname, argv[++i], sizeof(grp->ttifname) );
            continue;
        }
        else if(!strcasecmp("hwaddr",argv[i]) && (i+1) < argc)
        {
            MAC mac;
            if(grp->tthwaddr) {
                free(grp->tthwaddr);
                grp->tthwaddr = NULL;
            }
            if (ParseMAC( argv[i+1], mac ) == 0)
                grp->tthwaddr = strdup(argv[i+1]);
            ++i;
            continue;
        }
        else if(!strcasecmp("ipaddr",argv[i]) && (i+1) < argc)
        {
            char            *slash, *prfx;
            int             prfxsz;
            uint32_t        mask;
            struct in_addr  addr4;
            char            netmask[24];

            if(grp->ttipaddr)
                free(grp->ttipaddr);
            if(grp->ttpfxlen) {
                free(grp->ttpfxlen);
                grp->ttpfxlen = NULL;
            }
            slash = strchr( argv[i+1], '/' );  /* Point to slash character */
            if (slash) {                       /* If there is a slash      */
                prfx = slash + 1;              /* Point to prefix size     */
                prfxsz = atoi(prfx);
                if (( prfxsz >= 0 ) && ( prfxsz <= 32 )) {
                    switch( prfxsz )
                    {
                    case 0:
                        mask = 0x00000000;
                        break;
                    case 32:
                        mask = 0xFFFFFFFF;
                        break;
                    default:
                        mask = 0xFFFFFFFF ^ ( 0xFFFFFFFF >> prfxsz );
                        break;
                    }
                    addr4.s_addr = htonl(mask);
                    hinet_ntop( AF_INET, &addr4, netmask, sizeof(netmask) );

                    if(grp->ttnetmask)
                        free(grp->ttnetmask);
                    grp->ttnetmask = strdup(netmask);

                    grp->ttpfxlen = strdup(prfx);

                    slash[0] = 0;              /* Replace slash with null  */
                }
            }
            grp->ttipaddr = strdup(argv[++i]);
            continue;
        }
        else if(!strcasecmp("netmask",argv[i]) && (i+1) < argc)
        {
            if(grp->ttnetmask)
                free(grp->ttnetmask);
            grp->ttnetmask = strdup(argv[++i]);
            continue;
        }
#if defined(ENABLE_IPV6)
        else if(!strcasecmp("ipaddr6",argv[i]) && (i+1) < argc)
        {
            char  *slash, *prfx;
            int   prfxsz;

            if(grp->ttipaddr6)
                free(grp->ttipaddr6);
            if(grp->ttpfxlen6)
                free(grp->ttpfxlen6);
            slash = strchr( argv[i+1], '/' );  /* Point to slash character */
            if (slash) {                       /* If there is a slash      */
                prfx = slash + 1;              /* Point to prefix size     */
                prfxsz = atoi(prfx);
                if (( prfxsz >= 0 ) && ( prfxsz <= 128 )) {
                    slash[0] = 0;              /* Replace slash with null  */
                }
                else {
                    prfx = "128";
                }
            }
            else {
                prfx = "128";
            }
            grp->ttpfxlen6 = strdup(prfx);
            grp->ttipaddr6 = strdup(argv[++i]);
            continue;
        }
#endif /*defined(ENABLE_IPV6)*/
        else if(!strcasecmp("mtu",argv[i]) && (i+1) < argc)
        {
            if(grp->ttmtu)
                free(grp->ttmtu);
            grp->ttmtu = strdup(argv[++i]);
            continue;
        }
        else if(!strcasecmp("chpid",argv[i]) && (i+1) < argc)
        {
            int chpid;
            char c;
            if(sscanf(argv[++i], "%x%c", &chpid, &c) != 1 || chpid < 0x00 || chpid > 0xFF)
                logmsg(_("Invalid channel path id %s for device %4.4X\n"),argv[i],dev->devnum);

            else
                dev->pmcw.chpid[0] = chpid;

            continue;
        }
        else if (!strcasecmp("debug",argv[i]))
        {
            grp->debug = 1;
            continue;
        }
        else if(!strcasecmp("nodebug",argv[i]))
        {
            grp->debug = 0;
            continue;
        }
        else
            logmsg(_("Invalid option %s for device %4.4X\n"),argv[i],dev->devnum);
    }

    dev->fd = -1;
    dev->bufsize = 0xFFFF;      /* maximum packet/frame size */

    if (grouped)
    {
        DEVBLK *cua;
        U16 destlink;
        int i, pfxlen;

        /* Initialize each device's Full Link Address array */
        cua = dev->group->memdev[0];
        destlink = 0x000D; // ZZ FIXME: where should this come from?
        for(i = 0; i < OSA_GROUP_SIZE; i++) {
            dev->group->memdev[i]->fla[0] =
                (destlink << 8) | (cua->devnum & 0x00FF);
        }

        /* Initialize IPv4 mask field */
        if (grp->ttpfxlen)
            grp->pfxmask4 = (0xFFFFFFFF >> atoi(grp->ttpfxlen));
        else
            grp->pfxmask4 = 0xFFFFFFFF;

        /* Initialize IPv6 mask field */
        if (grp->ttpfxlen6 && (pfxlen = atoi(grp->ttpfxlen6)) >= 1 && pfxlen <= 128 )
        {
            int quo = pfxlen / 8;
            int rem = pfxlen % 8;
            memset( &grp->pfxmask6[0],   0x00,    quo );
            memset( &grp->pfxmask6[quo], 0xFF, 16-quo );
            grp->pfxmask6[quo] = (0xFF >> rem);
        }
        else
            memset( &grp->pfxmask6[0], 0xFF, 16 );
    }

    return 0;

} /* end function qeth_init_handler */


/*-------------------------------------------------------------------*/
/* Query the device definition                                       */
/*-------------------------------------------------------------------*/
static void qeth_query_device (DEVBLK *dev, char **devclass,
                int buflen, char *buffer)
{
char qdiostat[80] = {0};
char incomplete[16] = {0};
char status[sizeof(qdiostat)] = {0};
char active[8] = {0};
OSA_GRP *grp;

    BEGIN_DEVICE_CLASS_QUERY( "OSA", dev, devclass, buflen, buffer );

    grp = (OSA_GRP*)dev->group->grp_data;

    if (dev->group->acount == OSA_GROUP_SIZE)
    {
        char ttifname[IFNAMSIZ+2];

        strlcpy( ttifname, grp->ttifname, sizeof(ttifname));
        if (ttifname[0])
            strlcat( ttifname, " ", sizeof(ttifname));

        snprintf( qdiostat, sizeof(qdiostat), "%stx[%u] rx[%u] "
            , ttifname
            , dev->qdio.txcnt
            , dev->qdio.rxcnt
        );
    }

    if (dev->group->acount != OSA_GROUP_SIZE)
        strlcpy( incomplete, "*Incomplete ", sizeof( incomplete ));

    if (dev->scsw.flag2 & SCSW2_Q)
        strlcpy( status, qdiostat, sizeof( status ));

    if (dev->qdio.idxstate == MPC_IDX_STATE_ACTIVE)
        strlcpy( active, "IDX ", sizeof( active ));

    snprintf( buffer, buflen, "QDIO %s%s%s%sIO[%" I64_FMT "u]"
        , incomplete
        , status
        , active
        , grp ? (grp->debug ? "debug " : "") : ""
        , dev->excps
    );

} /* end function qeth_query_device */


/*-------------------------------------------------------------------*/
/* Close the device                                                  */
/*-------------------------------------------------------------------*/
static int qeth_close_device ( DEVBLK *dev )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

    if(!dev->member && dev->group->grp_data)
    {
        int ttfd = grp->ttfd;

        qeth_halt_device(dev);

        grp->ttfd = -1;
        dev->fd = -1;

        if(ttfd)
            TUNTAP_Close(ttfd);

        if(grp->ppfd[0])
            close_pipe(grp->ppfd[0]);
        if(grp->ppfd[1])
            close_pipe(grp->ppfd[1]);

        if(grp->tuntap)
            free(grp->tuntap);
        if(grp->tthwaddr)
            free(grp->tthwaddr);
        if(grp->ttipaddr)
            free(grp->ttipaddr);
        if(grp->ttpfxlen)
            free(grp->ttpfxlen);
        if(grp->ttnetmask)
            free(grp->ttnetmask);
        if(grp->ttipaddr6)
            free(grp->ttipaddr6);
        if(grp->ttpfxlen6)
            free(grp->ttpfxlen6);
        if(grp->ttmtu)
            free(grp->ttmtu);

        remove_and_free_any_buffers_on_chain( grp );

        destroy_condition(&grp->qcond);
        destroy_lock(&grp->qlock);
        destroy_lock(&grp->qblock);

        free(dev->group->grp_data);
        dev->group->grp_data = NULL;
    }
    else
        dev->fd = -1;
    return 0;
} /* end function qeth_close_device */


#if defined(_FEATURE_QDIO_THININT)
/*-------------------------------------------------------------------*/
/* QDIO Set Subchannel Indicator                                     */
/*-------------------------------------------------------------------*/
static int qeth_set_sci ( DEVBLK *dev, void *desc )
{
CHSC_REQ21 *req21 = (void *)desc;
RADR alsi, dsci;
BYTE ks, kc;
U16 opc;

    FETCH_HW(opc,req21->opcode);

    if(opc)
        return 3; // Invalid operation code

    FETCH_DW(alsi, req21->alsi);
    ks = req21->sk & CHSC_REQ21_KS;

    FETCH_DW(dsci, req21->dsci);
    kc = (req21->sk & CHSC_REQ21_KC) << 4;

    if(alsi && dsci)
    {
        if(STORCHK(alsi,0,ks,STORKEY_CHANGE,dev)
          || STORCHK(dsci,0,kc,STORKEY_CHANGE,dev))
        {
            dev->qdio.thinint = 0;
            return 3;
        }
        else
            dev->qdio.thinint = 1;

    }
    else
        dev->qdio.thinint = 0;

#if 0
    dev->pmcw.flag4 &= ~PMCW4_ISC;
    dev->pmcw.flag4 |= (req21->isc & CHSC_REQ21_ISC_MASK) << 3;
    dev->pmcw.flag25 &= ~PMCW25_VISC;
    dev->pmcw.flag25 |= (req21->isc & CHSC_REQ21_VISC_MASK) >> 4;
#endif

    dev->qdio.alsi = alsi;
    dev->qdio.ks = ks;

    dev->qdio.dsci = dsci;
    dev->qdio.kc = kc;

    return 0;
}
#endif /*defined(_FEATURE_QDIO_THININT)*/


/*-------------------------------------------------------------------*/
/* QDIO subsys desc                                                  */
/*-------------------------------------------------------------------*/
static int qeth_ssqd_desc ( DEVBLK *dev, void *desc )
{
    CHSC_RSP24 *rsp24 = (void *)desc;

    STORE_HW(rsp24->sch, dev->subchan);

    if(dev->pmcw.flag4 & PMCW4_Q)
    {
        rsp24->flags |= ( CHSC_FLAG_QDIO_CAPABILITY | CHSC_FLAG_VALIDITY );

        rsp24->qdioac1 |= ( AC1_SIGA_INPUT_NEEDED | AC1_SIGA_OUTPUT_NEEDED );
        rsp24->qdioac1 |= AC1_AUTOMATIC_SYNC_ON_OUT_PCI;

#if defined(_FEATURE_QEBSM)
        if(FACILITY_ENABLED_DEV(QEBSM))
        {
            STORE_DW(rsp24->sch_token, IOID2TKN((dev->ssid << 16) | dev->subchan));
            rsp24->qdioac1 |= ( AC1_SC_QEBSM_AVAILABLE | AC1_SC_QEBSM_ENABLED );
        }
#endif /*defined(_FEATURE_QEBSM)*/

#if defined(_FEATURE_QDIO_THININT)
        if(FACILITY_ENABLED_DEV(QDIO_THININT))
            rsp24->qdioac1 |= AC1_AUTOMATIC_SYNC_ON_THININT;
#endif /*defined(_FEATURE_QDIO_THININT)*/

        rsp24->icnt = QETH_QDIO_READQ;
        rsp24->ocnt = QETH_QDIO_WRITEQ;

        rsp24->qdioac1 |= AC1_UNKNOWN80;
        STORE_HW(rsp24->qdioac2, QETH_AC2_UNKNOWN4000+QETH_AC2_UNKNOWN2000);
    }

    return 0;
}


/*-------------------------------------------------------------------*/
/* Execute a Channel Command Word                                    */
/*-------------------------------------------------------------------*/
static void qeth_execute_ccw ( DEVBLK *dev, BYTE code, BYTE flags,
        BYTE chained, U16 count, BYTE prevcode, int ccwseq,
        BYTE *iobuf, BYTE *more, BYTE *unitstat, U16 *residual )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;
int num;                                /* Number of bytes to move   */

    UNREFERENCED(flags);
    UNREFERENCED(ccwseq);

    /* Clear the output */
    *more = 0;
    *unitstat = 0;
    *residual = 0;

    /* Command reject if the device group has not been established */
    if((dev->group->acount != OSA_GROUP_SIZE)
      && !(IS_CCW_SENSE(code) || IS_CCW_NOP(code) || (code == OSA_RCD)))
    {
        /* Set Intervention required sense, and unit check status */
        dev->sense[0] = SENSE_IR;
        *unitstat = CSW_CE | CSW_DE | CSW_UC;
        return;
    }

//! /* Display various information, maybe */
//! if( grp->debug )
//! {
//!     // HHC03992 "%1d:%04X %s: Code %02X: Flags %02X: Count %04X: Chained %02X: PrevCode %02X: CCWseq %d"
//!     WRMSG(HHC03992, "D", SSID_TO_LCSS(dev->ssid), dev->devnum, dev->typname,
//!         code, flags, count, chained, prevcode, ccwseq );
//! }

    /* Process depending on CCW opcode */
    switch (code) {


    case 0x01:
    /*---------------------------------------------------------------*/
    /* WRITE                                                         */
    /*---------------------------------------------------------------*/
    {
    int      datalen, length;
    U32      first4;

        /* Get the first 4-bytes of the data. */
        FETCH_FW( first4, iobuf );
        length = datalen = count;

        /* */
        if (first4 == MPC_TH_FIRST4)
        {
            /* Display the request MPC_TH etc., maybe. */
            if( grp->debug )
            {
                mpc_display_description( dev, "Request" );
                mpc_display_osa_th_etc( dev, (MPC_TH*)iobuf, FROM_GUEST, 0 );
            }
            /* Process the request MPC_TH etc. */
            osa_adapter_cmd(dev,(MPC_TH*)iobuf);
        }
        else if (first4 == MPC_IEA_FIRST4)
        {
            /* Display the IEA, maybe. */
            if( grp->debug )
            {
                mpc_display_description( dev, "Request" );
                mpc_display_osa_iea( dev, (MPC_IEA*)iobuf, FROM_GUEST, datalen );
            }
            /* Process the IEA. */
            osa_device_cmd(dev,(MPC_IEA*)iobuf,datalen);
        }
        else if (first4 == MPC_END_FIRST4)
        {
            /* Only ever seen during z/OS shutdown */
            if( grp->debug )
            {
                mpc_display_description( dev, "Shutdown Notify" );
                MPC_DUMP_DATA( "END", iobuf, length, FROM_GUEST );
            }
        }
        else
        {
            /* Display the unrecognised data. */
            mpc_display_description( dev, "Unrecognised Request" );
            if (length >= 256)
                length = 256;
            MPC_DUMP_DATA( "???", iobuf, length, FROM_GUEST );
        }

        /* Return normal status */
        *unitstat = CSW_CE | CSW_DE;
        *residual = 0;
        *more = 0;

        break;
    }


    case 0x02:
    /*---------------------------------------------------------------*/
    /* READ                                                          */
    /*---------------------------------------------------------------*/
    {
    OSA_BHR *bhr;
    BYTE    *iodata;
    int      datalen, length;
    U32      first4;
        /*
        ** Our purpose is to satisfy the program's request to read
        ** an IDX response. If there is a response queued (chained)
        ** then we return our response thereby satisfying the read,
        ** and exit with normal CSW status.
        **
        ** If we don't have an IDX response queued/chained though,
        ** then how we react depends on whether IDX handshaking is
        ** still active or not.
        **
        ** If IDX is not active (i.e. our dev->qdio.idxstate field
        ** is not MPC_IDX_STATE_ACTIVE) we return with a CSW status
        ** of unit check with status modifier (i.e. the read failed
        ** because there were no response buffers queued/chained to
        ** satisfy their read request with).
        **
        ** Otherwise as long as IDX is still active, we simply wait
        ** until a response is eventually queued (chained) so we can
        ** then use it to satisfy their read request with. That is
        ** to say, we will wait forever for a response to be queued
        ** as long as IDX is still active (we only exit when there
        ** is a response to give them or IDX is no longer active).
        */
        while (dev->qdio.idxstate == MPC_IDX_STATE_ACTIVE)
        {
            /* Remove IDX response buffer from chain. */
            bhr = remove_buffer_from_chain( grp );
            if (bhr)
            {
                /* Point to response data and get its length. */
                iodata = (BYTE*)bhr + SizeBHR;
                length = datalen = bhr->datalen;

                /* Set the residual length and normal status. */
                if (count >= datalen)
                {
                    *more     = 0;
                    *residual = count - datalen;
                }
                else
                {
                    datalen   = count;
                    *more     = 1;
                    *residual = 0;
                }
                *unitstat = CSW_CE | CSW_DE;

                /* What type of IDX response is this? */
                FETCH_FW( first4, iodata );

                if (first4 == MPC_TH_FIRST4)
                {
                    MPC_TH *th = (MPC_TH*)iodata;

                    /* Set the transmission header sequence number. */
                    STORE_FW( th->seqnum, ++grp->seqnumth );

                    /* Display the response MPC_TH etc., maybe. */
                    if( grp->debug )
                    {
                        mpc_display_description( dev, "Response" );
                        mpc_display_osa_th_etc( dev, (MPC_TH*)iodata, TO_GUEST, 0 );
                    }
                }
                else if (first4 == MPC_IEA_FIRST4)
                {
                    /* Display the IEAR, maybe. */
                    if( grp->debug )
                    {
                        mpc_display_description( dev, "Response" );
                        mpc_display_osa_iear( dev, (MPC_IEAR*)iodata, TO_GUEST, datalen );
                    }
                }
                else if (first4 == MPC_END_FIRST4)
                {
                    /* Only ever seen during z/OS shutdown */
                    if( grp->debug )
                    {
                        mpc_display_description( dev, "Shutdown Acknowledge" );
                        MPC_DUMP_DATA( "END", iobuf, length, TO_GUEST );
                    }
                }
                else
                {
                    /* Display the unrecognised data. */
                    mpc_display_description( dev, "Unrecognised Response" );
                    if (length >= 256)
                        length = 256;
                    MPC_DUMP_DATA( "???", iodata, length, TO_GUEST );
                }

                /* Copy IDX response data to i/o buffer. */
                memcpy( iobuf, iodata, datalen );

                /* Free IDX response buffer. Read is complete. */
                free( bhr );
                break; /*while*/
            }

            /* There are no IDX response buffers chained. */
            if(dev->qdio.idxstate != MPC_IDX_STATE_ACTIVE)
            {
                /* Return unit check with status modifier. */
                dev->sense[0] = 0;
                *unitstat = CSW_CE | CSW_DE | CSW_UC | CSW_SM;
                break; /*while*/
            }

            /* Wait for an IDX response buffer to be chained. */
            obtain_lock(&grp->qlock);
            wait_condition(&grp->qcond, &grp->qlock);
            release_lock(&grp->qlock);

        } /* end while (dev->qdio.idxstate == MPC_IDX_STATE_ACTIVE) */

        break; /*switch*/

    } /* end case 0x02: READ */


    case 0x03:
    /*---------------------------------------------------------------*/
    /* CONTROL NO-OPERATION                                          */
    /*---------------------------------------------------------------*/

        *residual = 0;
        *unitstat = CSW_CE | CSW_DE;
        break;


    case 0x14:
    /*---------------------------------------------------------------*/
    /* SENSE COMMAND BYTE                                            */
    /*---------------------------------------------------------------*/
    {
        /* We currently do not support emulated 3088 CTCA mode */
        dev->sense[0] = SENSE_CR;
        *unitstat = CSW_CE | CSW_DE | CSW_UC;
        break;
    }

    case 0x04:
    /*---------------------------------------------------------------*/
    /* SENSE                                                         */
    /*---------------------------------------------------------------*/

        /* Calculate residual byte count */
        num = (count < dev->numsense) ? count : dev->numsense;
        *residual = count - num;
//???   if (count < dev->numsense) *more = 1;

        /* Copy device sense bytes to channel I/O buffer */
        memcpy (iobuf, dev->sense, num);

        /* Clear the device sense bytes */
        memset (dev->sense, 0, sizeof(dev->sense));

        /* Return unit status */
        *unitstat = CSW_CE | CSW_DE;
        break;


    case 0xE4:
    /*---------------------------------------------------------------*/
    /* SENSE ID                                                      */
    /*---------------------------------------------------------------*/

        /* Calculate residual byte count */
        num = (count < dev->numdevid) ? count : dev->numdevid;
        *residual = count - num;
//???   if (count < dev->numdevid) *more = 1;

        /* Copy device identifier bytes to channel I/O buffer */
        memcpy (iobuf, dev->devid, num);

        /* Return unit status */
        *unitstat = CSW_CE | CSW_DE;

        /* Display formatted Sense Id information, maybe */
        if( grp->debug )
        {
            char buf[1024];
            // HHC03995 "%1d:%04X %s: %s:\n%s"
            WRMSG(HHC03995, "D", SSID_TO_LCSS(dev->ssid), dev->devnum,
                dev->typname, "SID", FormatSID( iobuf, num, buf, sizeof( buf )));
//          MPC_DUMP_DATA( "SID", iobuf, num, ' ' );
        }
        break;


    case OSA_RCD:
    /*---------------------------------------------------------------*/
    /* READ CONFIGURATION DATA                                       */
    /*---------------------------------------------------------------*/
    {
        int len = sizeof(configuration_data);
        NED *dev_ned = (NED*)iobuf;     /* Device NED is first       */
        NED *ctl_ned = dev_ned + 1;     /* Control Unit NED is next  */
        NED *tkn_ned = ctl_ned + 1;     /* Token NED is last NED     */
        NEQ *gen_neq = (NEQ*)tkn_ned+1; /* General NEQ always last   */
        DEVBLK *cua;                    /* Our Control Unit device   */

        /* Copy configuration data from tempate */
        memcpy (iobuf, configuration_data, len);

        /* The first device in the group is the control unit */
        cua = dev->group->memdev[0];

        /* Insert the Channel Path ID (CHPID) into all of the NEDs */
        dev_ned->tag[0] = dev->pmcw.chpid[0];
        ctl_ned->tag[0] = cua->pmcw.chpid[0];
        tkn_ned->tag[0] = cua->pmcw.chpid[0];

        /* Insert the device's device number into its device NED. */
        dev_ned->tag[1] = dev->devnum & 0xFF;

        /* Insert the control unit address into the General NEQ */
        gen_neq->iid[0] = cua->pmcw.chpid[0];
        gen_neq->iid[1] = cua->devnum & 0xFF;

        /* Calculate residual byte count */
        num = (count < len ? count : len);
        *residual = count - num;
        if (count < len) *more = 1;

        /* Return unit status */
        *unitstat = CSW_CE | CSW_DE;

        /* Display formatted Read Configuration Data records, maybe */
        if( grp->debug )
        {
            char buf[1024];
            // HHC03995 "%1d:%04X %s: %s:\n%s"
            WRMSG(HHC03995, "D", SSID_TO_LCSS(dev->ssid), dev->devnum,
                dev->typname, "RCD", FormatRCD( iobuf, num, buf, sizeof( buf )));
//          MPC_DUMP_DATA( "RCD", iobuf, num, ' ' );
        }
        break;
    }


    case OSA_SII:
    /*---------------------------------------------------------------*/
    /* SET INTERFACE IDENTIFIER                                      */
    /*---------------------------------------------------------------*/
    {
        // ZZ FIXME: PROGRAMMING NOTE: z/VM appears to always reject
        // this command so for the time being so will we. Since we're
        // not 100% sure about this however (we may later determine
        // that we actually need it), we shall simply disable it via
        // temporary #if statements. Once we know for certain we can
        // then remove the #ifs and keep the only code we need.

#if 0   // ZZ FIXME: should we be doing this?

        /* z/VM 5.3 always rejects this command so we will too */
        dev->sense[0] = SENSE_CR;
        *unitstat = CSW_CE | CSW_DE | CSW_UC;
        break;

#else   // ZZ FIXME: or should we be doing this instead?

        U32 iir;                    /* Work area to validate IIR     */
        FETCH_FW(iir,iobuf);        /* Fetch IIR into work area      */

        /* Command Reject if the Interface ID Record is invalid.
           Note: we only support one interface with an ID of 0. */
        if ((iir & 0xFFFCFFFF) != 0xB0000000 ||
            (iir & 0x00030000) == 0x00030000)
        {
            dev->sense[0] = SENSE_CR;
            *unitstat = CSW_CE | CSW_DE | CSW_UC;
            break;
        }

        /* Save the requested Interface ID for later unless it's
           not chained (to a presumably following RNI command) */
        if (chained)
            grp->iir = iir;

        /* Calculate residual byte count */
        num = (count < SII_SIZE) ? count : SII_SIZE;
        *residual = count - num;
        if (count < SII_SIZE) *more = 1;

        /* Return unit status */
        *unitstat = CSW_CE | CSW_DE;

        /* Display various information, maybe */
        if( grp->debug )
            MPC_DUMP_DATA( "SII", iobuf, num, ' ' );

        break;

#endif // ZZ FIXME
    }


    case OSA_RNI:
    /*---------------------------------------------------------------*/
    /* READ NODE IDENTIFIER                                          */
    /*---------------------------------------------------------------*/
    {
        int len = sizeof(node_data);
        ND *nd = (ND*)iobuf;            /* Node Descriptor pointer   */
        DEVBLK *cua;                    /* Our Control Unit device   */

        /* Command Reject if not chained from Set Interface ID */
        if (!chained || prevcode != OSA_SII)
        {
            dev->sense[0] = SENSE_CR;
            *unitstat = CSW_CE | CSW_DE | CSW_UC;
            break;
        }

        /* The first device in the group is the control unit */
        cua = dev->group->memdev[0];

        /* If the Node Selector was zero an ND and one or more
           NQs are returned. Otherwise just the ND is returned. */
        if ((grp->iir & 0x00030000) != 0)
            len = sizeof(ND);

        /* Copy configuration data from tempate */
        memcpy (iobuf, node_data, len);

        /* Insert the CHPID of the node into the Node Descriptor ND */
        nd->tag[0] = dev->pmcw.chpid[0];

        /* Update the Node Qualifier information if they want it */
        if (len > (int)sizeof(ND))
        {
            NQ *nq = (NQ*)nd + 1;       /* Point to Node Qualifier */

            /* Insert the CULA CHPID and device number into the NQ */
            nq->rsrvd[1] = cua->pmcw.chpid[0];
            nq->rsrvd[2] = cua->devnum & 0xFF;
        }

        /* Calculate residual byte count */
        num = (count < len ? count : len);
        *residual = count - num;
        if (count < len) *more = 1;

        /* Return unit status */
        *unitstat = CSW_CE | CSW_DE;

        /* Display formatted Read Node Information, maybe */
        if( grp->debug )
        {
            char buf[1024];
            // HHC03995 "%1d:%04X %s: %s:\n%s"
            WRMSG(HHC03995, "D", SSID_TO_LCSS(dev->ssid), dev->devnum,
                dev->typname, "RNI", FormatRNI( iobuf, num, buf, sizeof( buf )));
//          MPC_DUMP_DATA( "RNI", iobuf, num, ' ' );
        }
        break;
    }


    case OSA_EQ:
    /*---------------------------------------------------------------*/
    /* ESTABLISH QUEUES                                              */
    /*---------------------------------------------------------------*/
    {
        QDIO_QDR *qdr = (QDIO_QDR*)iobuf;
        QDIO_QDES0 *qdes;
        int accerr;
        int i;

        dev->qdio.i_qcnt = qdr->iqdcnt < QDIO_MAXQ ? qdr->iqdcnt : QDIO_MAXQ;
        dev->qdio.o_qcnt = qdr->oqdcnt < QDIO_MAXQ ? qdr->oqdcnt : QDIO_MAXQ;

        FETCH_DW(dev->qdio.qiba,qdr->qiba);
        dev->qdio.qibk = qdr->qkey & 0xF0;

        if(!(accerr = STORCHK(dev->qdio.qiba,sizeof(QDIO_QIB)-1,dev->qdio.qibk,STORKEY_CHANGE,dev)))
        {
        QDIO_QIB *qib = (QDIO_QIB*)(dev->mainstor + dev->qdio.qiba);
            qib->ac |= QIB_AC_PCI; // Incidate PCI on output is supported
#if defined(_FEATURE_QEBSM)
            if(FACILITY_ENABLED_DEV(QEBSM))
                qib->rflags |= QIB_RFLAGS_QEBSM;
#endif /*defined(_FEATURE_QEBSM)*/
        }

        qdes = qdr->qdf0;

        for(i = 0; i < dev->qdio.i_qcnt; i++)
        {
            FETCH_DW(dev->qdio.i_sliba[i],qdes->sliba);
            FETCH_DW(dev->qdio.i_sla[i],qdes->sla);
            FETCH_DW(dev->qdio.i_slsbla[i],qdes->slsba);
            dev->qdio.i_slibk[i] = qdes->keyp1 & 0xF0;
            dev->qdio.i_slk[i] = (qdes->keyp1 << 4) & 0xF0;
            dev->qdio.i_sbalk[i] = qdes->keyp2 & 0xF0;
            dev->qdio.i_slsblk[i] = (qdes->keyp2 << 4) & 0xF0;

            accerr |= STORCHK(dev->qdio.i_slsbla[i],sizeof(QDIO_SLSB)-1,dev->qdio.i_slsblk[i],STORKEY_CHANGE,dev);
            accerr |= STORCHK(dev->qdio.i_sla[i],sizeof(QDIO_SL)-1,dev->qdio.i_slk[i],STORKEY_REF,dev);

            qdes = (QDIO_QDES0*)((BYTE*)qdes+(qdr->iqdsz<<2));
        }

        for(i = 0; i < dev->qdio.o_qcnt; i++)
        {
            FETCH_DW(dev->qdio.o_sliba[i],qdes->sliba);
            FETCH_DW(dev->qdio.o_sla[i],qdes->sla);
            FETCH_DW(dev->qdio.o_slsbla[i],qdes->slsba);
            dev->qdio.o_slibk[i] = qdes->keyp1 & 0xF0;
            dev->qdio.o_slk[i] = (qdes->keyp1 << 4) & 0xF0;
            dev->qdio.o_sbalk[i] = qdes->keyp2 & 0xF0;
            dev->qdio.o_slsblk[i] = (qdes->keyp2 << 4) & 0xF0;

            accerr |= STORCHK(dev->qdio.o_slsbla[i],sizeof(QDIO_SLSB)-1,dev->qdio.o_slsblk[i],STORKEY_CHANGE,dev);
            accerr |= STORCHK(dev->qdio.o_sla[i],sizeof(QDIO_SL)-1,dev->qdio.o_slk[i],STORKEY_REF,dev);

            qdes = (QDIO_QDES0*)((BYTE*)qdes+(qdr->oqdsz<<2));
        }

        /* Calculate residual byte count */
        num = (count < sizeof(QDIO_QDR)) ? count : sizeof(QDIO_QDR);
        *residual = count - num;
        if (count < sizeof(QDIO_QDR)) *more = 1;

        if(!accerr)
        {
            /* Return unit status */
            *unitstat = CSW_CE | CSW_DE;
        }
        else
        {
            /* Command reject on invalid or inaccessible storage addresses */
            dev->sense[0] = SENSE_CR;
            *unitstat = CSW_CE | CSW_DE | CSW_UC;
        }

        break;
    }


    case OSA_AQ:
    /*---------------------------------------------------------------*/
    /* ACTIVATE QUEUES                                               */
    /*---------------------------------------------------------------*/
    {
    fd_set readset;                         /* select read set       */
    struct timeval tv;                      /* select polling        */
    int fd;                                 /* select fd             */
    int rc=0;                               /* select rc (0=timeout) */
    BYTE sig;                               /* thread pipe signal    */

        /*
        ** PROGRAMMING NOTE: we use a relatively short timeout value
        ** for our select so that we can react fairly quickly to the
        ** guest readying (priming) additional output buffers in its
        ** existing Output Queue(s) because a SIGA-w is not required.
        */
        tv.tv_sec  = 0;
        tv.tv_usec = 50000;                 /* 50 milliseconds       */
        dev->scsw.flag2 |= SCSW2_Q;         /* Indicate QDIO active  */
        dev->qdio.i_qmask = 0;              /* No input queues yet   */
        dev->qdio.o_qmask = 0;              /* No output queues yet  */
        FD_ZERO( &readset );                /* Init empty read set   */

        PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "beg act que", 0,0,0 );

        /* Loop until halt signal is received via notification pipe */
        while (1)
        {
            /* Read pipe signal if one was sent */
            sig = QDSIG_RESET;
            if(FD_ISSET(grp->ppfd[0],&readset))
            {
                read_pipe(grp->ppfd[0],&sig,1);
                DBGTRC(dev, "Activate Queues: signal %d received\n",sig);

                /* Exit immediately if requested to do so */
                if (sig == QDSIG_HALT)
                    break;

                /* Update packing mode flags if requested */
                if (QDSIG_READ   == sig) grp->rdpack = 0;
                if (QDSIG_RDMULT == sig) grp->rdpack = 1;
                if (QDSIG_WRIT   == sig) grp->wrpack = 0;
                if (QDSIG_WRMULT == sig) grp->wrpack = 1;
            }

            /* Process the Input Queue if any packets have arrived */
            if(rc != 0 && dev->qdio.i_qmask && FD_ISSET(grp->ttfd,&readset))
            {
                PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "b4 procinpq", 0,0,0 );
                process_input_queues(dev);
                PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "af procinpq", 0,0,0 );
            }

            /* ALWAYS process all Output Queues each time regardless of
               whether the guest has recently executed a SIGA-w or not
               since most guests expect OSA devices to behave that way.
               (SIGA-w are NOT required to cause processing o/p queues)
            */
            if(dev->qdio.o_qmask)
            {
                PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "b4 procoutq", 0,0,0 );
                process_output_queues(dev);
                PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "af procoutq", 0,0,0 );
            }

            /* Present adapter interrupt if needed */
            if(grp->reqpci)
            {
                grp->reqpci = FALSE;
                raise_adapter_interrupt(dev);
            }

            /* Prepare to wait for additional packets or pipe signal */
            fd = grp->ppfd[0];
            FD_ZERO( &readset );
            FD_SET( grp->ppfd[0], &readset );
            if(dev->qdio.i_qmask)
            {
                FD_SET( grp->ttfd, &readset );
                if (fd < grp->ttfd)
                    fd = grp->ttfd;
            }

            /* Wait (but only very briefly) for more work to arrive */
            PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "b4 select", 0,0,0 );
            rc = select( fd+1, &readset, NULL, NULL, &tv );
            PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "af select", 0,0,rc );
        }
        PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "end act que", 0,0,rc );

        /* Reply to halt signal */
        if (sig == QDSIG_HALT)
        {
            BYTE sig = QDSIG_HALT;
            write_pipe(grp->ppfd[1],&sig,1);
        }

        /* Return unit status */
        *unitstat = CSW_CE | CSW_DE;
        break;
    }


    default:
    /*---------------------------------------------------------------*/
    /* INVALID OPERATION                                             */
    /*---------------------------------------------------------------*/
        DBGTRC(dev, "Unknown CCW dev(%4.4x) code(%2.2x)\n",dev->devnum,code);
        /* Set command reject sense byte, and unit check status */
        dev->sense[0] = SENSE_CR;
        *unitstat = CSW_CE | CSW_DE | CSW_UC;

    } /* end switch(code) */

//! /* Display various information, maybe */
//! if( grp->debug )
//! {
//!     // HHC03993 "%1d:%04X %s: Status %02X: Residual %04X: More %02X"
//!     WRMSG(HHC03993, "D", SSID_TO_LCSS(dev->ssid), dev->devnum, dev->typname,
//!         *unitstat, *residual, *more );
//! }

} /* end function qeth_execute_ccw */


/*-------------------------------------------------------------------*/
/* Signal Adapter Initiate Input                                     */
/*-------------------------------------------------------------------*/
static int qeth_initiate_input(DEVBLK *dev, U32 qmask)
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;
int noselrd, rc = 0;

    DBGTRC( dev, "SIGA-r dev(%4.4x) qmask(%8.8x)\n", dev->devnum, qmask );
    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "b4 SIGA-r", qmask, dev->qdio.i_qmask, dev->devnum );

    /* Return CC1 if the device is not QDIO active */
    if(!(dev->scsw.flag2 & SCSW2_Q))
    {
        DBGTRC( dev, "SIGA-r dev(%4.4x): ERROR: QDIO not active\n", dev->devnum );
        rc = 1;
    }
    else
    {
        /* Is there a read select */
        noselrd = !dev->qdio.i_qmask;

        /* Validate Mask */
        qmask &= ~(0xffffffff >> dev->qdio.i_qcnt);

        /* Reset Queue Positions */
        if(qmask != dev->qdio.i_qmask)
        {
        int n;
            for(n = 0; n < dev->qdio.i_qcnt; n++)
                if(!(dev->qdio.i_qmask & (0x80000000 >> n)))
                    dev->qdio.i_bpos[n] = 0;
            if(!dev->qdio.i_qmask)
                dev->qdio.i_qpos = 0;

            /* Update Read Queue Mask */
            dev->qdio.i_qmask = qmask;
        }

        /* Send signal to ACTIVATE QUEUES device thread loop */
        if(noselrd && dev->qdio.i_qmask)
        {
            BYTE b = QDSIG_READ;
            write_pipe(grp->ppfd[1],&b,1);
        }
    }

    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "af SIGA-r", qmask, dev->qdio.i_qmask, dev->devnum );
    return rc;
}


/*-------------------------------------------------------------------*/
/* Signal Adapter Initiate Output/Multiple helper function           */
/*-------------------------------------------------------------------*/
static int qeth_do_initiate_output( DEVBLK *dev, U32 qmask, BYTE sig )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

    /* Return CC1 if the device is not QDIO active */
    if(!(dev->scsw.flag2 & SCSW2_Q))
        return 1;

    /* Validate Mask */
    qmask &= ~(0xffffffff >> dev->qdio.o_qcnt);

    /* Reset Queue Positions */
    if(qmask != dev->qdio.o_qmask)
    {
    int n;
        for(n = 0; n < dev->qdio.o_qcnt; n++)
            if(!(dev->qdio.o_qmask & (0x80000000 >> n)))
                dev->qdio.o_bpos[n] = 0;
        if(!dev->qdio.o_qmask)
            dev->qdio.o_qpos = 0;

        /* Update Write Queue Mask */
        dev->qdio.o_qmask = qmask;
    }

    /* Send signal to ACTIVATE QUEUES device thread loop */
    if(dev->qdio.o_qmask)
        write_pipe(grp->ppfd[1],&sig,1);

    return 0;
}


/*-------------------------------------------------------------------*/
/* Signal Adapter Initiate Output                                    */
/*-------------------------------------------------------------------*/
static int qeth_initiate_output( DEVBLK *dev, U32 qmask )
{
    int rc;
    DBGTRC( dev, "SIGA-w dev(%4.4x) qmask(%8.8x)\n", dev->devnum, qmask );
    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "b4 SIGA-w", qmask, dev->qdio.o_qmask, dev->devnum );

    if ((rc = qeth_do_initiate_output( dev, qmask, QDSIG_WRIT )) == 1)
        DBGTRC( dev, "SIGA-w dev(%4.4x): ERROR: QDIO not active\n", dev->devnum );

    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "af SIGA-w", qmask, dev->qdio.o_qmask, dev->devnum );
    return rc;
}


/*-------------------------------------------------------------------*/
/* Signal Adapter Initiate Output Multiple                           */
/*-------------------------------------------------------------------*/
static int qeth_initiate_output_mult( DEVBLK *dev, U32 qmask )
{
    int rc;
    DBGTRC( dev, "SIGA-m dev(%4.4x) qmask(%8.8x)\n", dev->devnum, qmask );
    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "b4 SIGA-m", qmask, dev->qdio.o_qmask, dev->devnum );

    if ((rc = qeth_do_initiate_output( dev, qmask, QDSIG_WRMULT )) == 1)
        DBGTRC( dev, "SIGA-m dev(%4.4x): ERROR: QDIO not active\n", dev->devnum );

    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "af SIGA-m", qmask, dev->qdio.o_qmask, dev->devnum );
    return rc;
}


/*-------------------------------------------------------------------*/
/* Signal Adapter Sync                                               */
/*-------------------------------------------------------------------*/
static int qeth_do_sync( DEVBLK *dev, U32 qmask )
{
    int rc = 0;
    UNREFERENCED(dev);          /* unreferenced for non-DEBUG builds */
    UNREFERENCED(qmask);        /* unreferenced for non-DEBUG builds */

    DBGTRC( dev, "SIGA-s dev(%4.4x) qmask(%8.8x)\n", dev->devnum, qmask );
    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "b4 SIGA-s", qmask, 0, dev->devnum );

    /* Return CC1 if the device is not QDIO active */
    if(!(dev->scsw.flag2 & SCSW2_Q))
    {
        DBGTRC( dev, "SIGA-s dev(%4.4x): ERROR: QDIO not active\n", dev->devnum );
        rc = 1;
    }
    else
    {
        /* (nop; do nothing) */
    }

    PTT_QETH_TIMING_DEBUG( PTT_CL_INF, "af SIGA-s", qmask, 0, dev->devnum );
    return rc;
}


/*-------------------------------------------------------------------*/
/* Process the CM_ENABLE request from the guest.                     */
/*-------------------------------------------------------------------*/
OSA_BHR* process_cm_enable( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

MPC_PUS *req_pus_01;
MPC_PUS *req_pus_02;

OSA_BHR *rsp_bhr;
MPC_TH  *rsp_th;
MPC_RRH *rsp_rrh;
MPC_PH  *rsp_ph;
MPC_PUK *rsp_puk;
MPC_PUS *rsp_pus_01;
MPC_PUS *rsp_pus_02;

U32 uLength1;
U32 uLength2;
U32 uLength3;
U16 uLength4;

    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);

    /* Point to the expected MPC_PUS and check they are present. */
    req_pus_01 = mpc_point_pus( dev, req_puk, PUS_TYPE_01 );
    req_pus_02 = mpc_point_pus( dev, req_puk, PUS_TYPE_02 );
    if( !req_pus_01 || !req_pus_02 )
    {
        /* FIXME Expected pus not present, error message please. */
        DBGTRC(dev, "process_cm_enable: Expected pus not present\n");
        return NULL;
    }

    /* Copy the guests CM Filter token from request PUS_TYPE_01. */
    memcpy( grp->gtcmfilt, req_pus_01->vc.pus_01.token, MPC_TOKEN_LENGTH );

    // Fix-up various lengths
    uLength4 = SIZE_PUS_01 +                     // first MPC_PUS
               SIZE_PUS_02_C;                    // second MPC_PUS
    uLength3 = SIZE_PUK + uLength4;              // the MPC_PUK and the MPC_PUSs (the data)
    uLength2 = SIZE_TH + SIZE_RRH_1 + SIZE_PH;   // the MPC_TH/MPC_RRH/MPC_PH
    uLength1 = uLength2 + uLength3;              // the MPC_TH/MPC_RRH/MPC_PH and data

    // Allocate a buffer in which the response will be build.
    rsp_bhr = alloc_buffer( dev, uLength1+10 );
    if (!rsp_bhr)
        return NULL;
    rsp_bhr->datalen = uLength1;

    // Fix-up various pointers
    rsp_th = (MPC_TH*)((BYTE*)rsp_bhr + SizeBHR);
    rsp_rrh = (MPC_RRH*)((BYTE*)rsp_th + SIZE_TH);
    rsp_ph = (MPC_PH*)((BYTE*)rsp_rrh + SIZE_RRH_1);
    rsp_puk = (MPC_PUK*)((BYTE*)rsp_ph + SIZE_PH);
    rsp_pus_01 = (MPC_PUS*)((BYTE*)rsp_puk + SIZE_PUK);
    rsp_pus_02 = (MPC_PUS*)((BYTE*)rsp_pus_01 + SIZE_PUS_01);

    // Prepare MPC_TH
    STORE_FW( rsp_th->first4, MPC_TH_FIRST4 );
    STORE_FW( rsp_th->offrrh, SIZE_TH );
    STORE_FW( rsp_th->length, uLength1 );
    STORE_HW( rsp_th->unknown10, 0x0FFC );            /* !!! */
    STORE_HW( rsp_th->numrrh, 1 );

    // Prepare MPC_RRH
    rsp_rrh->type = RRH_TYPE_CM;
    rsp_rrh->proto = PROTOCOL_UNKNOWN;
    STORE_HW( rsp_rrh->numph, 1 );
//  STORE_FW( rsp_rrh->seqnum, ++grp->seqnumis );
    STORE_HW( rsp_rrh->offph, SIZE_RRH_1 );
    STORE_HW( rsp_rrh->lenfida, (U16)uLength3 );
    STORE_F3( rsp_rrh->lenalda, uLength3 );
    rsp_rrh->tokenx5 = MPC_TOKEN_X5;
    memcpy( rsp_rrh->token, grp->gtissue, MPC_TOKEN_LENGTH );

    // Prepare MPC_PH
    rsp_ph->locdata = PH_LOC_1;
    STORE_F3( rsp_ph->lendata, uLength3 );
    STORE_FW( rsp_ph->offdata, uLength2 );

    // Prepare MPC_PUK
    STORE_HW( rsp_puk->length, SIZE_PUK );
    rsp_puk->what = PUK_WHAT_41;
    rsp_puk->type = PUK_TYPE_ENABLE;
    STORE_HW( rsp_puk->lenpus, uLength4 );

    // Prepare first MPC_PUS
    STORE_HW( rsp_pus_01->length, SIZE_PUS_01 );
    rsp_pus_01->what = PUS_WHAT_04;
    rsp_pus_01->type = PUS_TYPE_01;
    rsp_pus_01->vc.pus_01.proto = PROTOCOL_UNKNOWN;
    rsp_pus_01->vc.pus_01.unknown05 = 0x04;           /* !!! */
    rsp_pus_01->vc.pus_01.tokenx5 = MPC_TOKEN_X5;
    STORE_FW( rsp_pus_01->vc.pus_01.token, QTOKEN2 );

    // Prepare second MPC_PUS (which will contain 8-bytes of nulls)
    STORE_HW( rsp_pus_02->length, SIZE_PUS_02_C );
    rsp_pus_02->what = PUS_WHAT_04;
    rsp_pus_02->type = PUS_TYPE_02;

    return rsp_bhr;
}

/*-------------------------------------------------------------------*/
/* Process the CM_SETUP request from the guest.                      */
/*-------------------------------------------------------------------*/
OSA_BHR* process_cm_setup( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

MPC_PUS *req_pus_04;
MPC_PUS *req_pus_06;

OSA_BHR *rsp_bhr;
MPC_TH  *rsp_th;
MPC_RRH *rsp_rrh;
MPC_PH  *rsp_ph;
MPC_PUK *rsp_puk;
MPC_PUS *rsp_pus_04;
MPC_PUS *rsp_pus_08;
MPC_PUS *rsp_pus_07;

U32 uLength1;
U32 uLength2;
U32 uLength3;
U16 uLength4;

    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);

    /* Point to the expected MPC_PUS and check they are present. */
    req_pus_04 = mpc_point_pus( dev, req_puk, PUS_TYPE_04 );
    req_pus_06 = mpc_point_pus( dev, req_puk, PUS_TYPE_06 );
    if( !req_pus_04 || !req_pus_06 )
    {
        /* FIXME Expected pus not present, error message please. */
        DBGTRC(dev, "process_cm_setup: Expected pus not present\n");
        return NULL;
    }

    /* Copy the guests CM Connection token from request PUS_TYPE_04. */
    memcpy( grp->gtcmconn, req_pus_04->vc.pus_04.token, MPC_TOKEN_LENGTH );

    // Fix-up various lengths
    uLength4 = SIZE_PUS_04 +                     // first MPC_PUS
               SIZE_PUS_08 +                     // second MPC_PUS
               SIZE_PUS_07;                      // third MPC_PUS
    uLength3 = SIZE_PUK + uLength4;              // the MPC_PUK and the MPC_PUSs (the data)
    uLength2 = SIZE_TH + SIZE_RRH_1 + SIZE_PH;   // the MPC_TH/MPC_RRH/MPC_PH
    uLength1 = uLength2 + uLength3;              // the MPC_TH/MPC_RRH/MPC_PH and data

    // Allocate a buffer in which the response will be build.
    rsp_bhr = alloc_buffer( dev, uLength1+10 );
    if (!rsp_bhr)
        return NULL;
    rsp_bhr->datalen = uLength1;

    // Fix-up various pointers
    rsp_th = (MPC_TH*)((BYTE*)rsp_bhr + SizeBHR);
    rsp_rrh = (MPC_RRH*)((BYTE*)rsp_th + SIZE_TH);
    rsp_ph = (MPC_PH*)((BYTE*)rsp_rrh + SIZE_RRH_1);
    rsp_puk = (MPC_PUK*)((BYTE*)rsp_ph + SIZE_PH);
    rsp_pus_04 = (MPC_PUS*)((BYTE*)rsp_puk + SIZE_PUK);
    rsp_pus_08 = (MPC_PUS*)((BYTE*)rsp_pus_04 + SIZE_PUS_04);
    rsp_pus_07 = (MPC_PUS*)((BYTE*)rsp_pus_08 + SIZE_PUS_08);

    // Prepare MPC_TH
    STORE_FW( rsp_th->first4, MPC_TH_FIRST4 );
    STORE_FW( rsp_th->offrrh, SIZE_TH );
    STORE_FW( rsp_th->length, uLength1 );
    STORE_HW( rsp_th->unknown10, 0x0FFC );            /* !!! */
    STORE_HW( rsp_th->numrrh, 1 );

    // Prepare MPC_RRH
    rsp_rrh->type = RRH_TYPE_CM;
    rsp_rrh->proto = PROTOCOL_UNKNOWN;
    STORE_HW( rsp_rrh->numph, 1 );
//  STORE_FW( rsp_rrh->seqnum, ++grp->seqnumis );
    STORE_HW( rsp_rrh->offph, SIZE_RRH_1 );
    STORE_HW( rsp_rrh->lenfida, (U16)uLength3 );
    STORE_F3( rsp_rrh->lenalda, uLength3 );
    rsp_rrh->tokenx5 = MPC_TOKEN_X5;
    memcpy( rsp_rrh->token, grp->gtissue, MPC_TOKEN_LENGTH );

    // Prepare MPC_PH
    rsp_ph->locdata = PH_LOC_1;
    STORE_F3( rsp_ph->lendata, uLength3 );
    STORE_FW( rsp_ph->offdata, uLength2 );

    // Prepare MPC_PUK
    STORE_HW( rsp_puk->length, SIZE_PUK );
    rsp_puk->what = PUK_WHAT_41;
    rsp_puk->type = PUK_TYPE_CONFIRM;
    STORE_HW( rsp_puk->lenpus, uLength4 );

    // Prepare first MPC_PUS
    STORE_HW( rsp_pus_04->length, SIZE_PUS_04 );
    rsp_pus_04->what = PUS_WHAT_04;
    rsp_pus_04->type = PUS_TYPE_04;
    rsp_pus_04->vc.pus_04.tokenx5 = MPC_TOKEN_X5;
    memcpy( rsp_pus_04->vc.pus_04.token, grp->gtcmconn, MPC_TOKEN_LENGTH );

    // Prepare second MPC_PUS
    STORE_HW( rsp_pus_08->length, SIZE_PUS_08 );
    rsp_pus_08->what = PUS_WHAT_04;
    rsp_pus_08->type = PUS_TYPE_08;
    rsp_pus_08->vc.pus_08.tokenx5 = MPC_TOKEN_X5;
    STORE_FW( rsp_pus_08->vc.pus_08.token, QTOKEN3 );

    // Prepare third MPC_PUS
    STORE_HW( rsp_pus_07->length, SIZE_PUS_07 );
    rsp_pus_07->what = PUS_WHAT_04;
    rsp_pus_07->type = PUS_TYPE_07;
//  memcpy( rsp_pus_07->vc.pus_07.unknown04+0, req_pus_06->vc.pus_06.unknown04, 2 );
    STORE_HW( rsp_pus_07->vc.pus_07.unknown04+0, 0x4000 );
    STORE_HW( rsp_pus_07->vc.pus_07.unknown04+2, 0x0000 );

    return rsp_bhr;
}

/*-------------------------------------------------------------------*/
/* Process the CM_TAKEDOWN request from the guest.                   */
/*-------------------------------------------------------------------*/
OSA_BHR* process_cm_takedown( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

    UNREFERENCED(grp);
    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);
    UNREFERENCED(req_puk);

    /* There will be no response. */

    return NULL;
}

/*-------------------------------------------------------------------*/
/* Process the CM_DISABLE request from the guest.                    */
/*-------------------------------------------------------------------*/
OSA_BHR* process_cm_disable( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

    UNREFERENCED(grp);
    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);
    UNREFERENCED(req_puk);

    /* There will be no response. */

    return NULL;
}

/*-------------------------------------------------------------------*/
/* Process the ULP_ENABLE request from the guest to extract values.  */
/*-------------------------------------------------------------------*/
int process_ulp_enable_extract( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

MPC_PUS *req_pus_01;
MPC_PUS *req_pus_0A;

    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);

    /* Point to the expected MPC_PUS and check they are present. */
    req_pus_01 = mpc_point_pus( dev, req_puk, PUS_TYPE_01 );
    req_pus_0A = mpc_point_pus( dev, req_puk, PUS_TYPE_0A );
    if( !req_pus_01 || !req_pus_0A )
    {
        /* FIXME Expected pus not present, error message please. */
        DBGTRC(dev, "process_ulp_enable: Expected pus not present\n");
        return -1;
    }

    /* Remember whether we are using layer2 or layer3. */
    grp->l3 = (req_pus_01->vc.pus_01.proto == PROTOCOL_LAYER3);

    return 0;
}

/*-------------------------------------------------------------------*/
/* Process the ULP_ENABLE request from the guest.                    */
/*-------------------------------------------------------------------*/
OSA_BHR* process_ulp_enable( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

MPC_PUS *req_pus_01;
MPC_PUS *req_pus_0A;

OSA_BHR *rsp_bhr;
MPC_TH  *rsp_th;
MPC_RRH *rsp_rrh;
MPC_PH  *rsp_ph;
MPC_PUK *rsp_puk;
MPC_PUS *rsp_pus_01;
MPC_PUS *rsp_pus_0A;

U16      len_req_pus_0A;
U16      len_rsp_pus_0A;

U32 uLength1;
U32 uLength2;
U32 uLength3;
U16 uLength4;

    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);

    /* Point to the expected MPC_PUS and check they are present. */
    req_pus_01 = mpc_point_pus( dev, req_puk, PUS_TYPE_01 );
    req_pus_0A = mpc_point_pus( dev, req_puk, PUS_TYPE_0A );
    if( !req_pus_01 || !req_pus_0A )
    {
        /* FIXME Expected pus not present, error message please. */
        DBGTRC(dev, "process_ulp_enable: Expected pus not present\n");
        return NULL;
    }

    /* Copy the guests ULP Filter token from request PUS_TYPE_01. */
    memcpy( grp->gtulpfilt, req_pus_01->vc.pus_01.token, MPC_TOKEN_LENGTH );

    /* Determine length of request and response PUS_TYPE_0A. */
    len_rsp_pus_0A = SIZE_PUS_0A_B;
    FETCH_HW( len_req_pus_0A, req_pus_0A->length);
    if( len_req_pus_0A > SIZE_PUS_0A_B )
        len_rsp_pus_0A = len_req_pus_0A;

    // Fix-up various lengths
    uLength4 = SIZE_PUS_01 +                     // first MPC_PUS
               len_rsp_pus_0A;                   // second MPC_PUS
    uLength3 = SIZE_PUK + uLength4;              // the MPC_PUK and the MPC_PUSs (the data)
    uLength2 = SIZE_TH + SIZE_RRH_1 + SIZE_PH;   // the MPC_TH/MPC_RRH/MPC_PH
    uLength1 = uLength2 + uLength3;              // the MPC_TH/MPC_RRH/MPC_PH and data

    // Allocate a buffer in which the response will be build.
    rsp_bhr = alloc_buffer( dev, uLength1+10 );
    if (!rsp_bhr)
        return NULL;
    rsp_bhr->datalen = uLength1;

    // Fix-up various pointers
    rsp_th = (MPC_TH*)((BYTE*)rsp_bhr + SizeBHR);
    rsp_rrh = (MPC_RRH*)((BYTE*)rsp_th + SIZE_TH);
    rsp_ph = (MPC_PH*)((BYTE*)rsp_rrh + SIZE_RRH_1);
    rsp_puk = (MPC_PUK*)((BYTE*)rsp_ph + SIZE_PH);
    rsp_pus_01 = (MPC_PUS*)((BYTE*)rsp_puk + SIZE_PUK);
    rsp_pus_0A = (MPC_PUS*)((BYTE*)rsp_pus_01 + SIZE_PUS_01);

    // Prepare MPC_TH
    STORE_FW( rsp_th->first4, MPC_TH_FIRST4 );
    STORE_FW( rsp_th->offrrh, SIZE_TH );
    STORE_FW( rsp_th->length, uLength1 );
    STORE_HW( rsp_th->unknown10, 0x0FFC );            /* !!! */
    STORE_HW( rsp_th->numrrh, 1 );

    // Prepare MPC_RRH
    rsp_rrh->type = RRH_TYPE_ULP;
//  rsp_rrh->proto = PROTOCOL_UNKNOWN;
    rsp_rrh->proto = PROTOCOL_02;
    STORE_HW( rsp_rrh->numph, 1 );
    STORE_FW( rsp_rrh->seqnum, ++grp->seqnumcm );
    memcpy( rsp_rrh->ackseq, req_rrh->seqnum, 4 );
    STORE_HW( rsp_rrh->offph, SIZE_RRH_1 );
    STORE_HW( rsp_rrh->lenfida, (U16)uLength3 );
    STORE_F3( rsp_rrh->lenalda, uLength3 );
    rsp_rrh->tokenx5 = MPC_TOKEN_X5;
    memcpy( rsp_rrh->token, grp->gtcmconn, MPC_TOKEN_LENGTH );

    // Prepare MPC_PH
    rsp_ph->locdata = PH_LOC_1;
    STORE_F3( rsp_ph->lendata, uLength3 );
    STORE_FW( rsp_ph->offdata, uLength2 );

    // Prepare MPC_PUK
    STORE_HW( rsp_puk->length, SIZE_PUK );
    rsp_puk->what = PUK_WHAT_41;
    rsp_puk->type = PUK_TYPE_ENABLE;
    STORE_HW( rsp_puk->lenpus, uLength4 );

    // Prepare first MPC_PUS
    STORE_HW( rsp_pus_01->length, SIZE_PUS_01 );
    rsp_pus_01->what = PUS_WHAT_04;
    rsp_pus_01->type = PUS_TYPE_01;
    rsp_pus_01->vc.pus_01.proto = req_pus_01->vc.pus_01.proto;
    rsp_pus_01->vc.pus_01.unknown05 = 0x04;           /* !!! */
    rsp_pus_01->vc.pus_01.tokenx5 = MPC_TOKEN_X5;
    STORE_FW( rsp_pus_01->vc.pus_01.token, QTOKEN4 );

    // Prepare second MPC_PUS
    memcpy( rsp_pus_0A, req_pus_0A, len_req_pus_0A );
    STORE_HW( rsp_pus_0A->length, len_rsp_pus_0A );
    STORE_HW( rsp_pus_0A->vc.pus_0A.mtu, grp->uMTU );
    rsp_pus_0A->vc.pus_0A.linktype = PUS_LINK_TYPE_FAST_ETH;

    return rsp_bhr;
}

/*-------------------------------------------------------------------*/
/* Process the ULP_SETUP request from the guest.                     */
/*-------------------------------------------------------------------*/
OSA_BHR* process_ulp_setup( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

MPC_PUS *req_pus_04;
//C_PUS *req_pus_05;
MPC_PUS *req_pus_06;
MPC_PUS *req_pus_0B;
U16      len_pus_0B;

OSA_BHR *rsp_bhr;
MPC_TH  *rsp_th;
MPC_RRH *rsp_rrh;
MPC_PH  *rsp_ph;
MPC_PUK *rsp_puk;
MPC_PUS *rsp_pus_04;
MPC_PUS *rsp_pus_08;
MPC_PUS *rsp_pus_07;
MPC_PUS *rsp_pus_0B;

U32 uLength1;
U32 uLength2;
U32 uLength3;
U16 uLength4;

    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);

    /* Point to the expected MPC_PUS and check they are present. */
    req_pus_04 = mpc_point_pus( dev, req_puk, PUS_TYPE_04 );
    req_pus_06 = mpc_point_pus( dev, req_puk, PUS_TYPE_06 );
    req_pus_0B = mpc_point_pus( dev, req_puk, PUS_TYPE_0B );
    if( !req_pus_04 || !req_pus_06 || !req_pus_0B )
    {
        /* FIXME Expected pus not present, error message please. */
        DBGTRC(dev, "process_ulp_setup: Expected pus not present\n");
        return NULL;
    }
    FETCH_HW( len_pus_0B, req_pus_0B->length);

    /* Copy the guests ULP Connection token from request PUS_TYPE_04. */
    memcpy( grp->gtulpconn, req_pus_04->vc.pus_04.token, MPC_TOKEN_LENGTH );

    // Fix-up various lengths
    uLength4 = SIZE_PUS_04 +                     // first MPC_PUS
               SIZE_PUS_08 +                     // second MPC_PUS
               SIZE_PUS_07 +                     // third MPC_PUS
               len_pus_0B;                       // fourth MPC_PUS
    uLength3 = SIZE_PUK + uLength4;              // the MPC_PUK and the MPC_PUSs (the data)
    uLength2 = SIZE_TH + SIZE_RRH_1 + SIZE_PH;   // the MPC_TH/MPC_RRH/MPC_PH
    uLength1 = uLength2 + uLength3;              // the MPC_TH/MPC_RRH/MPC_PH and data

    // Allocate a buffer in which the response will be build.
    rsp_bhr = alloc_buffer( dev, uLength1+10 );
    if (!rsp_bhr)
        return NULL;
    rsp_bhr->datalen = uLength1;

    // Fix-up various pointers
    rsp_th = (MPC_TH*)((BYTE*)rsp_bhr + SizeBHR);
    rsp_rrh = (MPC_RRH*)((BYTE*)rsp_th + SIZE_TH);
    rsp_ph = (MPC_PH*)((BYTE*)rsp_rrh + SIZE_RRH_1);
    rsp_puk = (MPC_PUK*)((BYTE*)rsp_ph + SIZE_PH);
    rsp_pus_04 = (MPC_PUS*)((BYTE*)rsp_puk + SIZE_PUK);
    rsp_pus_08 = (MPC_PUS*)((BYTE*)rsp_pus_04 + SIZE_PUS_04);
    rsp_pus_07 = (MPC_PUS*)((BYTE*)rsp_pus_08 + SIZE_PUS_08);
    rsp_pus_0B = (MPC_PUS*)((BYTE*)rsp_pus_07 + SIZE_PUS_07);

    // Prepare MPC_TH
    STORE_FW( rsp_th->first4, MPC_TH_FIRST4 );
    STORE_FW( rsp_th->offrrh, SIZE_TH );
    STORE_FW( rsp_th->length, uLength1 );
    STORE_HW( rsp_th->unknown10, 0x0FFC );            /* !!! */
    STORE_HW( rsp_th->numrrh, 1 );

    // Prepare MPC_RRH
    rsp_rrh->type = RRH_TYPE_ULP;
//  rsp_rrh->proto = PROTOCOL_UNKNOWN;
    rsp_rrh->proto = PROTOCOL_02;
    STORE_HW( rsp_rrh->numph, 1 );
    STORE_FW( rsp_rrh->seqnum, ++grp->seqnumcm );
    memcpy( rsp_rrh->ackseq, req_rrh->seqnum, 4 );
    STORE_HW( rsp_rrh->offph, SIZE_RRH_1 );
    STORE_HW( rsp_rrh->lenfida, (U16)uLength3 );
    STORE_F3( rsp_rrh->lenalda, uLength3 );
    rsp_rrh->tokenx5 = MPC_TOKEN_X5;
    memcpy( rsp_rrh->token, grp->gtcmconn, MPC_TOKEN_LENGTH );

    // Prepare MPC_PH
    rsp_ph->locdata = PH_LOC_1;
    STORE_F3( rsp_ph->lendata, uLength3 );
    STORE_FW( rsp_ph->offdata, uLength2 );

    // Prepare MPC_PUK
    STORE_HW( rsp_puk->length, SIZE_PUK );
    rsp_puk->what = PUK_WHAT_41;
    rsp_puk->type = PUK_TYPE_CONFIRM;
    STORE_HW( rsp_puk->lenpus, uLength4 );

    // Prepare first MPC_PUS
    STORE_HW( rsp_pus_04->length, SIZE_PUS_04 );
    rsp_pus_04->what = PUS_WHAT_04;
    rsp_pus_04->type = PUS_TYPE_04;
    rsp_pus_04->vc.pus_04.tokenx5 = MPC_TOKEN_X5;
    memcpy( rsp_pus_04->vc.pus_04.token, grp->gtulpconn, MPC_TOKEN_LENGTH );

    // Prepare second MPC_PUS
    STORE_HW( rsp_pus_08->length, SIZE_PUS_08 );
    rsp_pus_08->what = PUS_WHAT_04;
    rsp_pus_08->type = PUS_TYPE_08;
    rsp_pus_08->vc.pus_08.tokenx5 = MPC_TOKEN_X5;
    STORE_FW( rsp_pus_08->vc.pus_08.token, QTOKEN5 );

    // Prepare third MPC_PUS
    STORE_HW( rsp_pus_07->length, SIZE_PUS_07 );
    rsp_pus_07->what = PUS_WHAT_04;
    rsp_pus_07->type = PUS_TYPE_07;
    memcpy( rsp_pus_07->vc.pus_07.unknown04+0, req_pus_06->vc.pus_06.unknown04, 2 );
    STORE_HW( rsp_pus_07->vc.pus_07.unknown04+2, 0x0000 );

    // Prepare fourth MPC_PUS
    memcpy( rsp_pus_0B, req_pus_0B, len_pus_0B );

    return rsp_bhr;
}

/*-------------------------------------------------------------------*/
/* Process the DM_ACT request from the guest.                        */
/*-------------------------------------------------------------------*/
OSA_BHR* process_dm_act( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

//C_PUS *req_pus_04;

OSA_BHR *rsp_bhr;
MPC_TH  *rsp_th;
MPC_RRH *rsp_rrh;
MPC_PH  *rsp_ph;
MPC_PUK *rsp_puk;
MPC_PUS *rsp_pus_04;

U32 uLength1;
U32 uLength2;
U32 uLength3;
U16 uLength4;

    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);
    UNREFERENCED(req_puk);

    // Fix-up various lengths
    uLength4 = SIZE_PUS_04;                      // first MPC_PUS
    uLength3 = SIZE_PUK + uLength4;              // the MPC_PUK and the MPC_PUSs (the data)
    uLength2 = SIZE_TH + SIZE_RRH_1 + SIZE_PH;   // the MPC_TH/MPC_RRH/MPC_PH
    uLength1 = uLength2 + uLength3;              // the MPC_TH/MPC_RRH/MPC_PH and data

    // Allocate a buffer in which the response will be build.
    rsp_bhr = alloc_buffer( dev, uLength1+10 );
    if (!rsp_bhr)
        return NULL;
    rsp_bhr->datalen = uLength1;

    // Fix-up various pointers
    rsp_th = (MPC_TH*)((BYTE*)rsp_bhr + SizeBHR);
    rsp_rrh = (MPC_RRH*)((BYTE*)rsp_th + SIZE_TH);
    rsp_ph = (MPC_PH*)((BYTE*)rsp_rrh + SIZE_RRH_1);
    rsp_puk = (MPC_PUK*)((BYTE*)rsp_ph + SIZE_PH);
    rsp_pus_04 = (MPC_PUS*)((BYTE*)rsp_puk + SIZE_PUK);

    // Prepare MPC_TH
    STORE_FW( rsp_th->first4, MPC_TH_FIRST4 );
    STORE_FW( rsp_th->offrrh, SIZE_TH );
    STORE_FW( rsp_th->length, uLength1 );
    STORE_HW( rsp_th->unknown10, 0x0FFC );            /* !!! */
    STORE_HW( rsp_th->numrrh, 1 );

    // Prepare MPC_RRH
    rsp_rrh->type = RRH_TYPE_ULP;
//  rsp_rrh->proto = PROTOCOL_UNKNOWN;
    rsp_rrh->proto = PROTOCOL_02;
    STORE_HW( rsp_rrh->numph, 1 );
    STORE_FW( rsp_rrh->seqnum, ++grp->seqnumcm );
    memcpy( rsp_rrh->ackseq, req_rrh->seqnum, 4 );
    STORE_HW( rsp_rrh->offph, SIZE_RRH_1 );
    STORE_HW( rsp_rrh->lenfida, (U16)uLength3 );
    STORE_F3( rsp_rrh->lenalda, uLength3 );
    rsp_rrh->tokenx5 = MPC_TOKEN_X5;
    memcpy( rsp_rrh->token, grp->gtcmconn, MPC_TOKEN_LENGTH );

    // Prepare MPC_PH
    rsp_ph->locdata = PH_LOC_1;
    STORE_F3( rsp_ph->lendata, uLength3 );
    STORE_FW( rsp_ph->offdata, uLength2 );

    // Prepare MPC_PUK
    STORE_HW( rsp_puk->length, SIZE_PUK );
    rsp_puk->what = PUK_WHAT_43;
    rsp_puk->type = PUK_TYPE_ACTIVE;
    STORE_HW( rsp_puk->lenpus, uLength4 );

    // Prepare first MPC_PUS
    STORE_HW( rsp_pus_04->length, SIZE_PUS_04 );
    rsp_pus_04->what = PUS_WHAT_04;
    rsp_pus_04->type = PUS_TYPE_04;
    rsp_pus_04->vc.pus_04.tokenx5 = MPC_TOKEN_X5;
    memcpy( rsp_pus_04->vc.pus_04.token, grp->gtulpconn, MPC_TOKEN_LENGTH );

    return rsp_bhr;
}

/*-------------------------------------------------------------------*/
/* Process the ULP_TAKEDOWN request from the guest.                  */
/*-------------------------------------------------------------------*/
OSA_BHR* process_ulp_takedown( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

    UNREFERENCED(grp);
    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);
    UNREFERENCED(req_puk);

    /* There will be no response. */

    return NULL;
}

/*-------------------------------------------------------------------*/
/* Process the ULP_DISABLE request from the guest.                   */
/*-------------------------------------------------------------------*/
OSA_BHR* process_ulp_disable( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

    UNREFERENCED(grp);
    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);
    UNREFERENCED(req_puk);

    /* There will be a single MPC_PUS_03 containing the grp->gtcmfilt token */

    /* There will be no response. */

    return NULL;
}

/*-------------------------------------------------------------------*/
/* Process unknown from the guest.                                   */
/*-------------------------------------------------------------------*/
OSA_BHR* process_unknown_puk( DEVBLK* dev, MPC_TH* req_th, MPC_RRH* req_rrh, MPC_PUK* req_puk )
{
OSA_GRP *grp = (OSA_GRP*)dev->group->grp_data;

    UNREFERENCED(grp);
    UNREFERENCED(req_th);
    UNREFERENCED(req_rrh);
    UNREFERENCED(req_puk);

    /* FIXME Error message please. */
    DBGTRC(dev, "process_unknown_puk\n");

    /* There will be no response. */

    return NULL;
}


/*--------------------------------------------------------------------*/
/* alloc_buffer(): Allocate storage for a OSA_BHR and data            */
/*--------------------------------------------------------------------*/
OSA_BHR*  alloc_buffer( DEVBLK* dev, int size )
{
    OSA_BHR*   bhr;                        // OSA_BHR
    int        buflen;                     // Buffer length
    char       etext[40];                  // malloc error text

    // Allocate the buffer.
    buflen = SizeBHR + size;
    bhr = malloc( buflen );                // Allocate the buffer
    if (!bhr)                              // if the allocate was not successful...
    {
        // Report the bad news.
        MSGBUF( etext, "malloc(%n)", &buflen );
        // HHC03960 "%1d:%04X %s: error in function '%s': '%s'"
        WRMSG(HHC03960, "E", SSID_TO_LCSS(dev->ssid), dev->devnum, dev->typname,
                             etext, strerror(errno) );
        return NULL;
    }

    // Clear the buffer.
    memset( bhr, 0, buflen );
    bhr->arealen = size;

    return bhr;
}

/*--------------------------------------------------------------------*/
/* add_buffer_to_chain_and_signal_event(): Add OSA_BHR to end of chn. */
/*--------------------------------------------------------------------*/
void*    add_buffer_to_chain_and_signal_event( OSA_GRP* grp, OSA_BHR* bhr )
{
    add_buffer_to_chain( grp, bhr );

    obtain_lock( &grp->qlock );
    signal_condition( &grp->qcond );
    release_lock( &grp->qlock );

    return NULL;
}

/*--------------------------------------------------------------------*/
/* add_buffer_to_chain(): Add OSA_BHR to end of chain.                */
/*--------------------------------------------------------------------*/
void*    add_buffer_to_chain( OSA_GRP* grp, OSA_BHR* bhr )
{
    // Prepare OSA_BHR for adding to chain.
    if (!bhr)                              // Any OSA_BHR been passed?
        return NULL;
    bhr->next = NULL;                      // Clear the pointer to next OSA_BHR

    // Obtain the buffer chain lock.
    obtain_lock( &grp->qblock );

    // Add OSA_BHR to end of chain.
    if (grp->firstbhr)                     // if there are already OSA_BHRs
    {
        grp->lastbhr->next = bhr;          // Add the OSA_BHR to
        grp->lastbhr = bhr;                // the end of the chain
        grp->numbhr++;                     // Increment number of OSA_BHRs
    }
    else
    {
        grp->firstbhr = bhr;               // Make the OSA_BHR
        grp->lastbhr = bhr;                // the only OSA_BHR
        grp->numbhr = 1;                   // on the chain
    }

    // Release the buffer chain lock.
    release_lock( &grp->qblock );

    return NULL;
}

/*--------------------------------------------------------------------*/
/* remove_buffer_from_chain(): Remove OSA_BHR from start of chain.    */
/*--------------------------------------------------------------------*/
OSA_BHR*  remove_buffer_from_chain( OSA_GRP* grp )
{
    OSA_BHR*    bhr;                       // OSA_BHR

    // Obtain the buffer chain lock.
    obtain_lock( &grp->qblock );

    // Point to first OSA_BHR on the chain.
    bhr = grp->firstbhr;                   // Pointer to first OSA_BHR

    // Remove the first OSA_BHR from the chain, if there is one...
    if (bhr)                               // If there is a OSA_BHR
    {
        grp->firstbhr = bhr->next;         // Make the next the first OSA_BHR
        grp->numbhr--;                     // Decrement number of OSA_BHRs
        bhr->next = NULL;                  // Clear the pointer to next OSA_BHR
        if (!grp->firstbhr)                // if there are no more OSA_BHRs
        {
//          grp->firstbhr = NULL;          // Clear
            grp->lastbhr = NULL;           // the chain
            grp->numbhr = 0;               // pointers and count
        }
    }

    // Release the buffer chain lock.
    release_lock( &grp->qblock );

    return bhr;
}

/*--------------------------------------------------------------------*/
/* remove_and_free_any_buffers_on_chain(): Remove and free OSA_BHRs.  */
/*--------------------------------------------------------------------*/
void*    remove_and_free_any_buffers_on_chain( OSA_GRP* grp )
{
    OSA_BHR*    bhr;                       // OSA_BHR

    // Obtain the buffer chain lock.
    obtain_lock( &grp->qblock );

    // Remove and free the OSA_BHRs on the chain, if there are any...
    while( grp->firstbhr != NULL )
    {
        bhr = grp->firstbhr;               // Pointer to first OSA_BHR
        grp->firstbhr = bhr->next;         // Make the next the first OSA_BHR
        free( bhr );                       // Free the message buffer
    }

    // Reset the chain pointers.
    grp->firstbhr = NULL;                  // Clear
    grp->lastbhr = NULL;                   // the chain
    grp->numbhr = 0;                       // pointers and count

    // Release the buffer chain lock.
    release_lock( &grp->qblock );

    return NULL;

}


/*-------------------------------------------------------------------*/
/* Initialize MAC address                                            */
/*-------------------------------------------------------------------*/
void InitMACAddr( DEVBLK* dev, OSA_GRP* grp )
{
    static const BYTE zeromac[IFHWADDRLEN] = {0};
    char* tthwaddr;
    BYTE iMAC[ IFHWADDRLEN ];
    int rc = 0;

    /* Retrieve the MAC Address directly from the tuntap interface */
    rc = TUNTAP_GetMACAddr( grp->ttifname, &tthwaddr );

    /* Did we get what we wanted? */
    if (0
        || rc != 0
        || ParseMAC( tthwaddr, iMAC ) != 0
        || memcmp( iMAC, zeromac, IFHWADDRLEN ) == 0
    )
    {
        char szMAC[3*IFHWADDRLEN] = {0};
        UNREFERENCED(dev); /*(referenced in non-debug build)*/
        DBGTRC(dev, "** WARNING ** TUNTAP_GetMACAddr() failed! Using default.\n");
        if (tthwaddr)
            free( tthwaddr );
        build_herc_iface_mac( iMAC, NULL );
        MSGBUF( szMAC, "%02X:%02X:%02X:%02X:%02X:%02X",
            iMAC[0], iMAC[1], iMAC[2],
            iMAC[3], iMAC[4], iMAC[5] );
        tthwaddr = strdup( szMAC );
    }

    grp->tthwaddr = strdup( tthwaddr );
    memcpy( grp->iMAC, iMAC, IFHWADDRLEN );

    free( tthwaddr );
}


/*-------------------------------------------------------------------*/
/* Initialize MTU value                                              */
/*-------------------------------------------------------------------*/
void InitMTU( DEVBLK* dev, OSA_GRP* grp )
{
    char* ttmtu;
    U16 uMTU;
    int rc = 0;

    /* Retrieve the MTU value directly from the TUNTAP interface */
    rc = TUNTAP_GetMTU( grp->ttifname, &ttmtu );

    /* Did we get what we wanted? */
    if (0
        || rc != 0
        || !(uMTU = (U16) atoi( ttmtu ))
        || uMTU < (60    - 14)
        || uMTU > (65535 - 14)
    )
    {
        UNREFERENCED(dev); /*(referenced in non-debug build)*/
        DBGTRC(dev, "** WARNING ** TUNTAP_GetMTU() failed! Using default.\n");
        if (ttmtu)
            free( ttmtu );
        ttmtu = strdup( QETH_DEF_MTU );
        uMTU = (U16) atoi( ttmtu );
    }

    grp->ttmtu = strdup( ttmtu );
    grp->uMTU  = uMTU;

    free( ttmtu );
}


/*-------------------------------------------------------------------*/
/* Very important things                                             */
/*-------------------------------------------------------------------*/

#if defined( OPTION_DYNAMIC_LOAD )
static
#endif
DEVHND qeth_device_hndinfo =
{
        &qeth_init_handler,            /* Device Initialisation      */
        &qeth_execute_ccw,             /* Device CCW execute         */
        &qeth_close_device,            /* Device Close               */
        &qeth_query_device,            /* Device Query               */
        NULL,                          /* Device Extended Query      */
        NULL,                          /* Device Start channel pgm   */
        NULL,                          /* Device End channel pgm     */
        NULL,                          /* Device Resume channel pgm  */
        NULL,                          /* Device Suspend channel pgm */
        &qeth_halt_device,             /* Device Halt channel pgm    */
        NULL,                          /* Device Read                */
        NULL,                          /* Device Write               */
        NULL,                          /* Device Query used          */
        NULL,                          /* Device Reserve             */
        NULL,                          /* Device Release             */
        NULL,                          /* Device Attention           */
        qeth_immed_commands,           /* Immediate CCW Codes        */
        &qeth_initiate_input,          /* Signal Adapter Input       */
        &qeth_initiate_output,         /* Signal Adapter Output      */
        &qeth_do_sync,                 /* Signal Adapter Sync        */
        &qeth_initiate_output_mult,    /* Signal Adapter Output Mult */
        &qeth_ssqd_desc,               /* QDIO subsys desc           */
#if defined(_FEATURE_QDIO_THININT)
        &qeth_set_sci,                 /* QDIO set subchan ind       */
#else /*defined(_FEATURE_QDIO_THININT)*/
        NULL,                          /* QDIO set subchan ind       */
#endif /*defined(_FEATURE_QDIO_THININT)*/
        NULL,                          /* Hercules suspend           */
        NULL                           /* Hercules resume            */
};

/* Libtool static name colision resolution */
/* note : lt_dlopen will look for symbol & modulename_LTX_symbol */
#if !defined( HDL_BUILD_SHARED ) && defined( HDL_USE_LIBTOOL )
#define hdl_ddev hdtqeth_LTX_hdl_ddev
#define hdl_depc hdtqeth_LTX_hdl_depc
#define hdl_reso hdtqeth_LTX_hdl_reso
#define hdl_init hdtqeth_LTX_hdl_init
#define hdl_fini hdtqeth_LTX_hdl_fini
#endif

#if defined( OPTION_DYNAMIC_LOAD )

HDL_DEPENDENCY_SECTION;
{
    HDL_DEPENDENCY( HERCULES );
    HDL_DEPENDENCY( DEVBLK );
    HDL_DEPENDENCY( SYSBLK );

    memcpy( (NED*)&configuration_data[0], &osa_device_ned [0], sizeof( NED ));
    memcpy( (NED*)&configuration_data[1], &osa_ctlunit_ned[0], sizeof( NED ));
    memcpy( (NED*)&configuration_data[2], &osa_token_ned  [0], sizeof( NED ));
    memcpy( (NED*)&configuration_data[3], &osa_general_neq[0], sizeof( NEQ ));

    memcpy( (ND*)&node_data[0], &osa_nd[0], sizeof( ND ));
    memcpy( (ND*)&node_data[1], &osa_nq[0], sizeof( NQ ));
}
END_DEPENDENCY_SECTION


HDL_RESOLVER_SECTION;
{
  #if defined( WIN32 ) && !defined( _MSVC_ ) && !defined( HDL_USE_LIBTOOL )
    #undef sysblk
    HDL_RESOLVE_PTRVAR( psysblk, sysblk );
  #endif
}
END_RESOLVER_SECTION


HDL_REGISTER_SECTION;
{
//               Hercules's          Our
//               registered          overriding
//               entry-point         entry-point
//               name                value
#if defined( WIN32 )
  HDL_REGISTER ( debug_tt32_stats,   display_tt32_stats        );
  HDL_REGISTER ( debug_tt32_tracing, enable_tt32_debug_tracing );
#endif
}
END_REGISTER_SECTION


HDL_DEVICE_SECTION;
{
    HDL_DEVICE ( QETH, qeth_device_hndinfo );
}
END_DEVICE_SECTION

#endif // defined(OPTION_DYNAMIC_LOAD)

#if defined( _MSVC_ ) && defined( NO_QETH_OPTIMIZE )
  #pragma optimize( "", on )            // restore previous settings
#endif

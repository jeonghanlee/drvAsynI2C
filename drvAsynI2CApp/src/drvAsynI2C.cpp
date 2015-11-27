//******************************************************************************
// Copyright (C) 2015 Florian Feldbauer <florian@ep1.ruhr-uni-bochum.de>
//                    - University Mainz, Institute foer nuc
//
// This file is part of drvAsynCan
//
// drvAsynCan is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// drvAsynCan is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
// brief   AsynPortDriver for PANDA Raspberry Pi CAN interface
//
// version 3.0.0; Jul. 29, 2014
//******************************************************************************

//_____ I N C L U D E S _______________________________________________________

// ANSI C includes
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <sys/types.h>
//#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

// EPICS includes
#include <epicsEvent.h>
#include <epicsExport.h>
#include <epicsMutex.h>
#include <epicsString.h>
#include <epicsThread.h>
#include <epicsTime.h>
#include <epicsTimer.h>
#include <epicsTypes.h>
#include <iocsh.h>

// local includes
#include "drvAsynI2C.h"

//_____ D E F I N I T I O N S __________________________________________________

//_____ G L O B A L S __________________________________________________________

//_____ L O C A L S ____________________________________________________________
static epicsTimerId       i2c_timer;
static epicsTimerQueueId  i2c_timerQueue;

//_____ F U N C T I O N S ______________________________________________________

//------------------------------------------------------------------------------
//! @brief       Timeout handler for I2C communication 
//------------------------------------------------------------------------------
static void timeoutHandler( void *ptr ) {
  drvAsynI2C *pi2c = static_cast<drvAsynI2C*>( ptr );
  pi2c->timeout();
}

//------------------------------------------------------------------------------
//! @brief       Called when asyn clients call pasynOctet->read().
//! @param [in]  pasynUser  pasynUser structure that encodes the reason and address.
//! @param [out] value      Address of the string to read.
//! @param [in]  maxChars   Maximum number of characters to read.
//! @param [out] nActual    Number of characters actually read.
//! @param [out] eomReason  Reason that read terminated.
//! @return      in case of no error occured asynSuccess is returned. Otherwise
//!              asynError or asynTimeout is returned. A error message is stored
//!              in pasynUser->errorMessage.
//! @sa          writeOctet
//!
//! Read a byte stream from the I2C bus.
//! To set the correct slave address the pasynOctet->write() call has to be used.
//! If the slave has multiple registers, use the write call to setup slave
//! address and register address, followed by this read call.
///------------------------------------------------------------------------------
asynStatus drvAsynI2C::readOctet( asynUser *pasynUser, char *value, size_t maxChars,
                                  size_t *nActual, int *eomReason ) {
  if( _fd < 0 ) {
    epicsSnprintf( pasynUser->errorMessage, pasynUser->errorMessageSize,
                   "%s: %s disconnected:", portName, _deviceName );
    return asynError;
  }
  if( maxChars <= 0 ) {
    epicsSnprintf( pasynUser->errorMessage, pasynUser->errorMessageSize,
                   "%s: %s maxchars %d Why <=0?", portName, _deviceName, (int)maxchars );
    return asynError;
  }

  int thisRead = 0;
  int nRead = 0;
  bool timerStarted = false;
  asynStatus status = asynSuccess;

  _timeout = false;

  if( eomReason ) *eomReason = 0;

  for(;;) {
    if( !timerStarted && pasynUser->timeout > 0 ) {
      epicsTimerStartDelay( i2c_timer, pasynUser->timeout );
      timerStarted = true;
    }
    thisRead = read( _fd, value, maxChars );
    if( thisRead > 0 ) {
      nRead = thisRead;
      break;
    } else {
      if( thisRead < 0 && ( errno != EWOULDBLOCK )
                       && ( errno != EINTR )
                       && ( errno != EAGAIN ) ) {
        epicsSnprintf( pasynUser->errorMessage, pasynUser->errorMessageSize,
                       "%s: %s read error: %s",
                       portName, _deviceName, strerror( errno ) );
        status = asynError;
        break;
      }
    }
    if( _timeout ) break;
  }
  if( timerStarted ) epicsTimerCancel( i2c_timer );
  if( _timeout && asynSuccess == status ) status = asynTimeout;

/*
  int mytimeout = (int)( pasynUser->timeout * 1.e6 );
  if ( 0 > mytimeout ) {

    nbytes = read( _fd, value, maxChars );
    if ( 0 > nbytes ) {
      epicsSnprintf( pasynUser->errorMessage, pasynUser->errorMessageSize, 
                     "Error receiving message from device '%s': %d %s", 
                     _deviceName, errno, strerror( errno ) );
      return asynError;
    }

  } else {

    fd_set fdRead;
    struct timeval t;
    
    // calculate timeout values
    t.tv_sec  = mytimeout / 1000000L;
    t.tv_usec = mytimeout % 1000000L;
    
    FD_ZERO( &fdRead );
    FD_SET( _fd, &fdRead );
    
    // wait until timeout or a message is ready to get read
    int err = select( _fd + 1, &fdRead, NULL, NULL, &t );
    
    // the only one file descriptor is ready for read
    if ( 0 < err ) {
      nbytes = read( _fd, value, maxChars );
      if ( 0 > nbytes ) {
        epicsSnprintf( pasynUser->errorMessage, pasynUser->errorMessageSize, 
                       "Error receiving message from device '%s': %d %s", 
                       _deviceName, errno, strerror( errno ) );
        return asynError;
      }
    }
    
    // nothing is ready, timeout occured
    if ( 0 == err ) return asynTimeout;
    if ( 0 > err )  {
      epicsSnprintf( pasynUser->errorMessage, pasynUser->errorMessageSize, 
                     "Error receiving message from device '%s': %d %s", 
                     _deviceName, errno, strerror( errno ) );
      return asynError;
    }
  }
*/

  *nActual = nbytes;
  if( eomReason && nbytes >= maxChars ) {
    *eomReason = ASYN_EOM_CNT;
  }

  asynPrint( pasynUser, ASYN_TRACEIO_DRIVER, 
             "%s: read %lu bytes from %s, return %s\n",
             portName, (unsigned long)*nActual, _deviceName,
             pasynManager->strStatus( status ) );
  
  return status; 
}

//------------------------------------------------------------------------------
//! @brief       Called when asyn clients call pasynOctet->write().
//! @param [in]  pasynUser  pasynUser structure that encodes the reason and address.
//! @param [in]  value      Address of the string to write.
//! @param [in]  nChars     Number of characters to write.
//! @param [out] nActual    Number of characters actually written.
//! @return      in case of no error occured asynSuccess is returned. Otherwise
//!              asynError or asynTimeout is returned. A error message is stored
//!              in pasynUser->errorMessage.
//!
//! Write a byte stream to the i2c bus. First byte holds the slave address, followed
//! by the acutal data to send.
//! Example: Setting the configuration register of the AD7998 to convert all
//! 8 channels and use the BUSY output:
//! value = 0x20 0x02 0x0ffa
//! Depending on the slave device, multiple write commands can be concatenated
///------------------------------------------------------------------------------
asynStatus drvAsynI2C::writeOctet( asynUser *pasynUser, char const *value, size_t maxChars,
                                   size_t *nActual ){

  int thisWrite = 0;
  bool timerStarted = false;
  asynStatus status = asynSuccess;

  if( _fd < 0 ) {
    epicsSnprintf( pasynUser->errorMessage, pasynUser->errorMessageSize,
                   "%s: %s disconnected:", portName, _deviceName );
    return asynError;
  }
  if( 0 == maxChars ) {
    *nActual = 0;
    return asynSuccess;
  }

  _timeout = false;

  int addr = value[0];
  if( addr != _slaveAddress ) {
    // set slave address
   if( ioctl( _fd, I2C_SLAVE, addr ) < 0 ) {
     epicsSnprintf( pasynUser->errorMessage, pasynUser->errorMessageSize,
                    "%s: %s Can't set slave address: %s",
                    portName, _deviceName, strerror( errno ) );
     return asynError;

    _slaveAddress = addr;
   }
  ++value;
  int nleft = maxChars - 1;
  if( 0 < nleft ) {

    if( pasynUser->timeout > 0 ) {
      epicsTimerStartDelay( i2c_timer, pasynUser->timeout );
      timerStarted = true;
    }

    for(;;) {
      thisWrite = write( _fd, value, nleft );
      if ( 0 < thisWrite ) {
        nleft -= thisWrite;
        if( 0 == nleft ) break;
        value += thisWrite;
      }
      if( _timeout || 0 == pasynUser->timeout ) {
        status = asynTimeout;
        break;
      }
      if( 0 > thisWrite && ( errno != EWOULDBLOCK )
                        && ( errno != EINTR )
                        && ( errno != EAGAIN ) ) {
        epicsSnprintf( pasynUser->errorMessage, pasynUser->errorMessageSize,
                       "%s: %s write error: %s",
                       portName, _deviceName, strerror( errno ) );
        status = asynError;
        break;
      }
    }
    if( timerStarted ) epicsTimerCancel( i2c_timer );

  }

  *nActual = maxChars - nleft;

  asynPrint( pasynUser, ASYN_TRACEIO_DRIVER, 
             "%s: wrote %lu bytes to %s, return %s\n",
             portName, (unsigned long)*nActual, _deviceName,
             pasynManager->strStatus( status ) );
  
  return status; 
}

//------------------------------------------------------------------------------
//! @brief       Connect driver to device
//! @param [in]  pasynUser  pasynUser structure that encodes the reason and address.
//! @return      in case of no error occured asynSuccess is returned. Otherwise
//!              asynError or asynTimeout is returned. A error message is stored
//!              in pasynUser->errorMessage.
///------------------------------------------------------------------------------
asynStatus drvAsynI2C::connect( asynUser *pasynUser ) {
  if( _fd >= 0 ) {
    epicsSnprintf( pasynUser->errorMessage,pasynUser->errorMessageSize,
                   "%s: Link to %s already open!", portName, _deviceName );
    return asynError;
  }
  asynPrint( pasynUser, ASYN_TRACEIO_DRIVER,
             "%s: Open connection to %s\n", portName, _deviceName );

  if( ( _fd = open( _deviceName, O_RDWR ) ) < 0 ) {
    epicsSnprintf( pasynUser->errorMessage,pasynUser->errorMessageSize,
                   "%s: Can't open %s: %s", portName, _deviceName, strerror( errno ) );
    return asynError;
  }
  if( ioctl( _fd, I2C_FUNCS, _i2cfuncs ) < 0 ) {
    epicsSnprintf( pasynUser->errorMessage,pasynUser->errorMessageSize,
                   "%s: Can't get functionality of %s: %s", portName, _deviceName, strerror( errno ) );
    return asynError;
  }
  pasynManager->exceptionConnect( pasynUser );
  return asynSuccess;
}

//------------------------------------------------------------------------------
//! @brief       Disconnect driver from device
//! @param [in]  pasynUser  pasynUser structure that encodes the reason and address.
//! @return      in case of no error occured asynSuccess is returned. Otherwise
//!              asynError or asynTimeout is returned. A error message is stored
//!              in pasynUser->errorMessage.
///------------------------------------------------------------------------------
asynStatus drvAsynI2C::disconnect( asynUser *pasynUser ) {
  asynPrint( pasynUser, ASYN_TRACEIO_DRIVER,
             "%s: disconnect %s\n", portName, _deviceName );
  epicsTimerCancel( i2c_timer );
  if( _fd >= 0 ) {
    close( _fd );
    _fd = -1;
    pasynManager->exceptionDisconnect(pasynUser);
  } 
  return asynSuccess;
}

//------------------------------------------------------------------------------
//! @brief       Standard C'tor.
//! @param [in]  portName The name of the asynPortDriver to be created.
//! @param [in]  ttyName  The name of the device
//------------------------------------------------------------------------------
drvAsynI2C::drvAsynI2C( const char *portName, const char *ttyName ) 
  : asynPortDriver( portName,
                    1, // maxAddr
                    2,
                    asynCommonMask | asynOctetMask | asynDrvUserMask, // Interface mask
                    asynCommonMask | asynOctetMask,  // Interrupt mask
                    ASYN_CANBLOCK, // asynFlags
                    1,  // Autoconnect
                    0,  // Default priority
                    0 ) // Default stack size
{
  _deviceName = epicsStrDup( ttyName );
  _fd = -1;
  _slaveAddress = 0;
}

// Configuration routines.  Called directly, or from the iocsh function below 
extern "C" {
  //----------------------------------------------------------------------------
  //! @brief       EPICS iocsh callable function to call constructor
  //!              for the drvAsynI2C class.
  //! @param [in]  portName The name of the asyn port driver to be created.
  //! @param [in]  ttyName  The name of the interface 
  //----------------------------------------------------------------------------
  int drvAsynI2CConfigure( const char *portName, const char *ttyName ) {
    if( !portName ) {
      printf( "Port name missing.\n" );
      return -1;
    }
    if( !ttyName ) {
      printf( "TTY name missing.\n" );
      return -1;
    }
    drvAsynI2C* pi2c = new drvAsynCan( portName, ttyName );
    i2c_timerQueue = epicsTimerQueueAllocate( 1, epicsThreadPriorityScanLow );
    i2c_timer = epicsTimerQueueCreateTimer( i2c_timerQueue, timeoutHandler, pi2c );
    if( !i2c_timer ) {
      printf( "drvAsynI2C: Can't create timer.\n");
      return -1;
    }
    return( asynSuccess );
  }
  static const iocshArg initI2CArg0 = { "portName", iocshArgString };
  static const iocshArg initI2CArg1 = { "ttyName",  iocshArgString };
  static const iocshArg * const initI2CArgs[] = { &initI2CArg0, &initI2CArg1 };
  static const iocshFuncDef initI2CFuncDef = { "drvAsynI2CConfigure", 2, initI2CArgs };
  static void initI2CCallFunc( const iocshArgBuf *args ) {
    drvAsynI2CConfigure( args[0].sval, args[1].sval );
  }

  //----------------------------------------------------------------------------
  //! @brief   Register functions to EPICS
  //----------------------------------------------------------------------------
  void drvAsynI2CRegister( void ) {
    static int firstTime = 1;
    if ( firstTime ) {
      iocshRegister( &initI2CFuncDef, initI2CCallFunc );
      firstTime = 0;
    }
  }
  
  epicsExportRegistrar( drvAsynI2CRegister );
}

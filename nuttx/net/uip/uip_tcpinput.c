/****************************************************************************
 * net/uip/uip_tcpinput.c
 * Handling incoming TCP input
 *
 *   Copyright (C) 2007-2009 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <spudmonkey@racsa.co.cr>
 *
 * Adapted for NuttX from logic in uIP which also has a BSD-like license:
 *
 *   Original author Adam Dunkels <adam@dunkels.com>
 *   Copyright () 2001-2003, Adam Dunkels.
 *   All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#if defined(CONFIG_NET) && defined(CONFIG_NET_TCP)

#include <stdint.h>
#include <string.h>
#include <debug.h>

#include <net/uip/uipopt.h>
#include <net/uip/uip.h>
#include <net/uip/uip-arch.h>

#include "uip_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BUF ((struct uip_tcpip_hdr *)&dev->d_buf[UIP_LLH_LEN])

/****************************************************************************
 * Public Variables
 ****************************************************************************/

/****************************************************************************
 * Private Variables
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: uip_tcpinput
 *
 * Description:
 *   Handle incoming TCP input
 *
 * Parameters:
 *   dev - The device driver structure containing the received TCP packet.
 *
 * Return:
 *   None
 *
 * Assumptions:
 *   Called from the interrupt level or with interrupts disabled.
 *
 ****************************************************************************/

void uip_tcpinput(struct uip_driver_s *dev)
{
  struct uip_conn *conn = NULL;
  struct uip_tcpip_hdr *pbuf = BUF;
  uint16_t tmp16;
  uint16_t flags;
  uint8_t  opt;
  uint8_t  result;
  int      len;
  int      i;

  dev->d_snddata = &dev->d_buf[UIP_IPTCPH_LEN + UIP_LLH_LEN];
  dev->d_appdata = &dev->d_buf[UIP_IPTCPH_LEN + UIP_LLH_LEN];

#ifdef CONFIG_NET_STATISTICS
  uip_stat.tcp.recv++;
#endif

  /* Start of TCP input header processing code. */

  if (uip_tcpchksum(dev) != 0xffff)
    {
      /* Compute and check the TCP checksum. */

#ifdef CONFIG_NET_STATISTICS
      uip_stat.tcp.drop++;
      uip_stat.tcp.chkerr++;
#endif
      nlldbg("Bad TCP checksum\n");
      goto drop;
    }

  /* Demultiplex this segment. First check any active connections. */

  conn = uip_tcpactive(pbuf);
  if (conn)
    {
      goto found;
    }

  /* If we didn't find and active connection that expected the packet,
   * either (1) this packet is an old duplicate, or (2) this is a SYN packet
   * destined for a connection in LISTEN. If the SYN flag isn't set,
   * it is an old packet and we send a RST.
   */

  if ((pbuf->flags & TCP_CTL) == TCP_SYN)
    {
      /* This is a SYN packet for a connection.  Find the connection
       * listening on this port.
       */

      tmp16 = pbuf->destport;
      if (uip_islistener(tmp16))
        {
          /* We matched the incoming packet with a connection in LISTEN.
           * We now need to create a new connection and send a SYNACK in
           * response.
           */

          /* First allocate a new connection structure and see if there is any
           * user application to accept it.
           */

          conn = uip_tcpaccept(pbuf);
          if (conn)
            {
              /* The connection structure was successfully allocated.  Now see if
               * there is an application waiting to accept the connection (or at
               * least queue it it for acceptance).
               */

              conn->tcpstateflags = UIP_ESTABLISHED;
              conn->crefs         = 1;

              if (uip_accept(dev, conn, tmp16) != OK)
                {
                  /* No, then we have to give the connection back */

                  conn->crefs     = 0;
                  uip_tcpfree(conn);
                  conn = NULL;
                }
            }

          if (!conn)
            {
              /* Either (1) all available connections are in use, or (2) there is no
               * application in place to accept the connection.  We drop packet and hope that
               * the remote end will retransmit the packet at a time when we
               * have more spare connections or someone waiting to accept the connection.
               */

#ifdef CONFIG_NET_STATISTICS
              uip_stat.tcp.syndrop++;
#endif
              nlldbg("No free TCP connections\n");
              goto drop;
            }

          uip_incr32(conn->rcv_nxt, 1);

          /* Parse the TCP MSS option, if present. */

          if ((pbuf->tcpoffset & 0xf0) > 0x50)
            {
              for (i = 0; i < ((pbuf->tcpoffset >> 4) - 5) << 2 ;)
                {
                  opt = dev->d_buf[UIP_TCPIP_HLEN + UIP_LLH_LEN + i];
                  if (opt == TCP_OPT_END)
                    {
                      /* End of options. */

                      break;
                    }
                  else if (opt == TCP_OPT_NOOP)
                    {
                      /* NOP option. */

                      ++i;
                    }
                  else if (opt == TCP_OPT_MSS &&
                          dev->d_buf[UIP_TCPIP_HLEN + UIP_LLH_LEN + 1 + i] == TCP_OPT_MSS_LEN)
                    {
                      /* An MSS option with the right option length. */

                      tmp16 = ((uint16_t)dev->d_buf[UIP_TCPIP_HLEN + UIP_LLH_LEN + 2 + i] << 8) |
                               (uint16_t)dev->d_buf[UIP_IPTCPH_LEN + UIP_LLH_LEN + 3 + i];
                      conn->initialmss = conn->mss =
                              tmp16 > UIP_TCP_MSS? UIP_TCP_MSS: tmp16;

                      /* And we are done processing options. */

                      break;
                    }
                  else
                    {
                      /* All other options have a length field, so that we easily
                       * can skip past them.
                       */

                      if (dev->d_buf[UIP_TCPIP_HLEN + UIP_LLH_LEN + 1 + i] == 0)
                        {
                          /* If the length field is zero, the options are malformed
                           * and we don't process them further.
                           */

                          break;
                        }
                      i += dev->d_buf[UIP_TCPIP_HLEN + UIP_LLH_LEN + 1 + i];
                    }
                }
            }

          /* Our response will be a SYNACK. */

          uip_tcpack(dev, conn, TCP_ACK | TCP_SYN);
          return;
        }
    }

  /* This is (1) an old duplicate packet or (2) a SYN packet but with
   * no matching listener found.  Send RST packet in either case.
   */

  /* We do not send resets in response to resets. */

  if (pbuf->flags & TCP_RST)
    {
      goto drop;
    }

#ifdef CONFIG_NET_STATISTICS
  uip_stat.tcp.synrst++;
#endif
  uip_tcpreset(dev);
  return;

found:

  flags = 0;

  /* We do a very naive form of TCP reset processing; we just accept
   * any RST and kill our connection. We should in fact check if the
   * sequence number of this reset is wihtin our advertised window
   * before we accept the reset.
   */

  if (pbuf->flags & TCP_RST)
    {
      conn->tcpstateflags = UIP_CLOSED;
      nlldbg("RESET - TCP state: UIP_CLOSED\n");

      (void)uip_tcpcallback(dev, conn, UIP_ABORT);
      goto drop;
    }

  /* Calculated the length of the data, if the application has sent
   * any data to us.
   */

  len = (pbuf->tcpoffset >> 4) << 2;

  /* d_len will contain the length of the actual TCP data. This is
   * calculated by subtracting the length of the TCP header (in
   * len) and the length of the IP header (20 bytes).
   */

  dev->d_len -= (len + UIP_IPH_LEN);

  /* First, check if the sequence number of the incoming packet is
   * what we're expecting next. If not, we send out an ACK with the
   * correct numbers in.
   */

  if (!(((conn->tcpstateflags & UIP_TS_MASK) == UIP_SYN_SENT) &&
      ((pbuf->flags & TCP_CTL) == (TCP_SYN | TCP_ACK))))
    {
      if ((dev->d_len > 0 || ((pbuf->flags & (TCP_SYN | TCP_FIN)) != 0)) &&
          memcmp(pbuf->seqno, conn->rcv_nxt, 4) != 0)
        {
            uip_tcpsend(dev, conn, TCP_ACK, UIP_IPTCPH_LEN);
            return;
        }
    }

  /* Next, check if the incoming segment acknowledges any outstanding
   * data. If so, we update the sequence number, reset the length of
   * the outstanding data, calculate RTT estimations, and reset the
   * retransmission timer.
   */

  if ((pbuf->flags & TCP_ACK) && uip_outstanding(conn))
    {
      uint32_t seqno;
      uint32_t ackno;

      /* The next sequence number is equal to the current sequence
       * number (snd_nxt) plus the size of the oustanding data (len).
       */

      seqno = uip_tcpaddsequence(conn->snd_nxt, conn->len);

      /* Check if all of the outstanding bytes have been acknowledged. For
       * a "generic" send operation, this should always be true.  However,
       * the send() API sends data ahead when it can without waiting for
       * the ACK.  In this case, the 'ackno' could be less than then the
       * new sequence number.
       */

      ackno = uip_tcpgetsequence(pbuf->ackno);
      if (ackno <= seqno)
        {
          /* Update sequence number. */

          uip_tcpsetsequence(conn->snd_nxt, seqno);

          /* Do RTT estimation, unless we have done retransmissions. */

          if (conn->nrtx == 0)
            {
              signed char m;
              m = conn->rto - conn->timer;

              /* This is taken directly from VJs original code in his paper */

              m = m - (conn->sa >> 3);
              conn->sa += m;
              if (m < 0)
                {
                  m = -m;
                }

              m = m - (conn->sv >> 2);
              conn->sv += m;
              conn->rto = (conn->sa >> 3) + conn->sv;
            }

          /* Set the acknowledged flag. */

          flags = UIP_ACKDATA;

          /* Reset the retransmission timer. */

          conn->timer = conn->rto;

          /* Reset length of outstanding data. */

          conn->len = 0;
        }
    }

  /* Do different things depending on in what state the connection is. */

  switch (conn->tcpstateflags & UIP_TS_MASK)
    {
      /* CLOSED and LISTEN are not handled here. CLOSE_WAIT is not
       * implemented, since we force the application to close when the
       * peer sends a FIN (hence the application goes directly from
       * ESTABLISHED to LAST_ACK).
       */

      case UIP_SYN_RCVD:
        /* In SYN_RCVD we have sent out a SYNACK in response to a SYN, and
         * we are waiting for an ACK that acknowledges the data we sent
         * out the last time. Therefore, we want to have the UIP_ACKDATA
         * flag set. If so, we enter the ESTABLISHED state.
         */

        if (flags & UIP_ACKDATA)
          {
            conn->tcpstateflags = UIP_ESTABLISHED;
            conn->len           = 0;
            nllvdbg("TCP state: UIP_ESTABLISHED\n");

            flags               = UIP_CONNECTED;

            if (dev->d_len > 0)
              {
                flags          |= UIP_NEWDATA;
                uip_incr32(conn->rcv_nxt, dev->d_len);
              }

            dev->d_sndlen       = 0;
            result              = uip_tcpcallback(dev, conn, flags);
            uip_tcpappsend(dev, conn, result);
            return;
          }
        goto drop;

      case UIP_SYN_SENT:
        /* In SYN_SENT, we wait for a SYNACK that is sent in response to
         * our SYN. The rcv_nxt is set to sequence number in the SYNACK
         * plus one, and we send an ACK. We move into the ESTABLISHED
         * state.
         */

        if ((flags & UIP_ACKDATA) && (pbuf->flags & TCP_CTL) == (TCP_SYN | TCP_ACK))
          {
            /* Parse the TCP MSS option, if present. */

            if ((pbuf->tcpoffset & 0xf0) > 0x50)
              {
                for (i = 0; i < ((pbuf->tcpoffset >> 4) - 5) << 2 ;)
                  {
                    opt = dev->d_buf[UIP_IPTCPH_LEN + UIP_LLH_LEN + i];
                    if (opt == TCP_OPT_END)
                      {
                        /* End of options. */

                        break;
                      }
                    else if (opt == TCP_OPT_NOOP)
                      {
                        /* NOP option. */

                        ++i;
                      }
                    else if (opt == TCP_OPT_MSS &&
                              dev->d_buf[UIP_TCPIP_HLEN + UIP_LLH_LEN + 1 + i] == TCP_OPT_MSS_LEN)
                      {
                        /* An MSS option with the right option length. */

                        tmp16 =
                          (dev->d_buf[UIP_TCPIP_HLEN + UIP_LLH_LEN + 2 + i] << 8) |
                          dev->d_buf[UIP_TCPIP_HLEN + UIP_LLH_LEN + 3 + i];
                        conn->initialmss =
                          conn->mss =
                          tmp16 > UIP_TCP_MSS? UIP_TCP_MSS: tmp16;

                        /* And we are done processing options. */

                        break;
                      }
                    else
                      {
                        /* All other options have a length field, so that we
                         * easily can skip past them.
                         */

                        if (dev->d_buf[UIP_TCPIP_HLEN + UIP_LLH_LEN + 1 + i] == 0)
                          {
                            /* If the length field is zero, the options are
                             * malformed and we don't process them further.
                             */

                            break;
                          }
                        i += dev->d_buf[UIP_TCPIP_HLEN + UIP_LLH_LEN + 1 + i];
                      }
                  }
              }

            conn->tcpstateflags = UIP_ESTABLISHED;
            memcpy(conn->rcv_nxt, pbuf->seqno, 4);
            nllvdbg("TCP state: UIP_ESTABLISHED\n");

            uip_incr32(conn->rcv_nxt, 1);
            conn->len           = 0;
            dev->d_len          = 0;
            dev->d_sndlen       = 0;
            result = uip_tcpcallback(dev, conn, UIP_CONNECTED | UIP_NEWDATA);
            uip_tcpappsend(dev, conn, result);
            return;
          }

        /* Inform the application that the connection failed */

        (void)uip_tcpcallback(dev, conn, UIP_ABORT);

        /* The connection is closed after we send the RST */

        conn->tcpstateflags = UIP_CLOSED;
        nllvdbg("Connection failed - TCP state: UIP_CLOSED\n");

        /* We do not send resets in response to resets. */

        if (pbuf->flags & TCP_RST)
          {
            goto drop;
          }
        uip_tcpreset(dev);
        return;

      case UIP_ESTABLISHED:
        /* In the ESTABLISHED state, we call upon the application to feed
         * data into the d_buf. If the UIP_ACKDATA flag is set, the
         * application should put new data into the buffer, otherwise we are
         * retransmitting an old segment, and the application should put that
         * data into the buffer.
         *
         * If the incoming packet is a FIN, we should close the connection on
         * this side as well, and we send out a FIN and enter the LAST_ACK
         * state. We require that there is no outstanding data; otherwise the
         * sequence numbers will be screwed up.
         */

        if (pbuf->flags & TCP_FIN && !(conn->tcpstateflags & UIP_STOPPED))
          {
            if (uip_outstanding(conn))
              {
                goto drop;
              }

            uip_incr32(conn->rcv_nxt, dev->d_len + 1);
            flags |= UIP_CLOSE;

            if (dev->d_len > 0)
              {
                flags |= UIP_NEWDATA;
              }

            (void)uip_tcpcallback(dev, conn, flags);

            conn->tcpstateflags = UIP_LAST_ACK;
            conn->len           = 1;
            conn->nrtx          = 0;
            nllvdbg("TCP state: UIP_LAST_ACK\n");

            uip_tcpsend(dev, conn, TCP_FIN | TCP_ACK, UIP_IPTCPH_LEN);
            return;
          }

        /* Check the URG flag. If this is set, the segment carries urgent
         * data that we must pass to the application.
         */

        if ((pbuf->flags & TCP_URG) != 0)
          {
#ifdef CONFIG_NET_TCPURGDATA
            dev->d_urglen = (pbuf->urgp[0] << 8) | pbuf->urgp[1];
            if (dev->d_urglen > dev->d_len)
              {
                /* There is more urgent data in the next segment to come. */

                dev->d_urglen = dev->d_len;
              }

            uip_incr32(conn->rcv_nxt, dev->d_urglen);
            dev->d_len     -= dev->d_urglen;
            dev->d_urgdata  = dev->d_appdata;
            dev->d_appdata += dev->d_urglen;
          }
        else
          {
            dev->d_urglen   = 0;
#else /* CONFIG_NET_TCPURGDATA */
            dev->d_appdata =
              ((uint8_t*)dev->d_appdata) + ((pbuf->urgp[0] << 8) | pbuf->urgp[1]);
            dev->d_len    -=
              (pbuf->urgp[0] << 8) | pbuf->urgp[1];
#endif /* CONFIG_NET_TCPURGDATA */
          }

        /* If d_len > 0 we have TCP data in the packet, and we flag this
         * by setting the UIP_NEWDATA flag and update the sequence number
         * we acknowledge. If the application has stopped the dataflow
         * using uip_stop(), we must not accept any data packets from the
         * remote host.
         */

        if (dev->d_len > 0 && !(conn->tcpstateflags & UIP_STOPPED))
          {
            flags |= UIP_NEWDATA;
            uip_incr32(conn->rcv_nxt, dev->d_len);
          }

        /* Check if the available buffer space advertised by the other end
         * is smaller than the initial MSS for this connection. If so, we
         * set the current MSS to the window size to ensure that the
         * application does not send more data than the other end can
         * handle.
         *
         * If the remote host advertises a zero window, we set the MSS to
         * the initial MSS so that the application will send an entire MSS
         * of data. This data will not be acknowledged by the receiver,
         * and the application will retransmit it. This is called the
         * "persistent timer" and uses the retransmission mechanim.
         */

        tmp16 = ((uint16_t)pbuf->wnd[0] << 8) + (uint16_t)pbuf->wnd[1];
        if (tmp16 > conn->initialmss || tmp16 == 0)
          {
            tmp16 = conn->initialmss;
          }
        conn->mss = tmp16;

        /* If this packet constitutes an ACK for outstanding data (flagged
         * by the UIP_ACKDATA flag, we should call the application since it
         * might want to send more data. If the incoming packet had data
         * from the peer (as flagged by the UIP_NEWDATA flag), the
         * application must also be notified.
         *
         * When the application is called, the d_len field
         * contains the length of the incoming data. The application can
         * access the incoming data through the global pointer
         * d_appdata, which usually points UIP_IPTCPH_LEN + UIP_LLH_LEN
         *  bytes into the d_buf array.
         *
         * If the application wishes to send any data, this data should be
         * put into the d_appdata and the length of the data should be
         * put into d_len. If the application don't have any data to
         * send, d_len must be set to 0.
         */

        if (flags & (UIP_NEWDATA | UIP_ACKDATA))
          {
            dev->d_sndlen = 0;
            result        = uip_tcpcallback(dev, conn, flags);
            uip_tcpappsend(dev, conn, result);
            return;
          }
        goto drop;

      case UIP_LAST_ACK:
        /* We can close this connection if the peer has acknowledged our
         * FIN. This is indicated by the UIP_ACKDATA flag.
         */

        if (flags & UIP_ACKDATA)
          {
            conn->tcpstateflags = UIP_CLOSED;
            nllvdbg("UIP_LAST_ACK TCP state: UIP_CLOSED\n");

            (void)uip_tcpcallback(dev, conn, UIP_CLOSE);
          }
        break;

      case UIP_FIN_WAIT_1:
        /* The application has closed the connection, but the remote host
         * hasn't closed its end yet. Thus we do nothing but wait for a
         * FIN from the other side.
         */

        if (dev->d_len > 0)
          {
            uip_incr32(conn->rcv_nxt, dev->d_len);
          }
        if (pbuf->flags & TCP_FIN)
          {
            if (flags & UIP_ACKDATA)
              {
                conn->tcpstateflags = UIP_TIME_WAIT;
                conn->timer         = 0;
                conn->len           = 0;
                nllvdbg("TCP state: UIP_TIME_WAIT\n");
              }
            else
              {
                conn->tcpstateflags = UIP_CLOSING;
                nllvdbg("TCP state: UIP_CLOSING\n");
              }

            uip_incr32(conn->rcv_nxt, 1);
            (void)uip_tcpcallback(dev, conn, UIP_CLOSE);
            uip_tcpsend(dev, conn, TCP_ACK, UIP_IPTCPH_LEN);
            return;
          }
        else if (flags & UIP_ACKDATA)
          {
            conn->tcpstateflags = UIP_FIN_WAIT_2;
            conn->len = 0;
            nllvdbg("TCP state: UIP_FIN_WAIT_2\n");
            goto drop;
          }

        if (dev->d_len > 0)
          {
            uip_tcpsend(dev, conn, TCP_ACK, UIP_IPTCPH_LEN);
            return;
          }
        goto drop;

      case UIP_FIN_WAIT_2:
        if (dev->d_len > 0)
          {
            uip_incr32(conn->rcv_nxt, dev->d_len);
          }

        if (pbuf->flags & TCP_FIN)
          {
            conn->tcpstateflags = UIP_TIME_WAIT;
            conn->timer         = 0;
            nllvdbg("TCP state: UIP_TIME_WAIT\n");

            uip_incr32(conn->rcv_nxt, 1);
            (void)uip_tcpcallback(dev, conn, UIP_CLOSE);
            uip_tcpsend(dev, conn, TCP_ACK, UIP_IPTCPH_LEN);
            return;
          }

        if (dev->d_len > 0)
          {
            uip_tcpsend(dev, conn, TCP_ACK, UIP_IPTCPH_LEN);
            return;
          }
        goto drop;

      case UIP_TIME_WAIT:
        uip_tcpsend(dev, conn, TCP_ACK, UIP_IPTCPH_LEN);
        return;

      case UIP_CLOSING:
        if (flags & UIP_ACKDATA)
          {
            conn->tcpstateflags = UIP_TIME_WAIT;
            conn->timer        = 0;
            nllvdbg("TCP state: UIP_TIME_WAIT\n");
          }

      default:
        break;
    }

drop:
  dev->d_len = 0;
}

#endif /* CONFIG_NET  && CONFIG_NET_TCP */

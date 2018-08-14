/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2010,2011  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/net/netbuff.h>
#include <grub/dl.h>
#include <grub/net.h>
#include <grub/time.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/i18n.h>
#include <grub/net/ip.h>

GRUB_MOD_LICENSE ("GPLv3+");

/* GUID.  */
static grub_efi_guid_t net_io_guid = GRUB_EFI_SIMPLE_NETWORK_GUID;
static grub_efi_guid_t pxe_io_guid = GRUB_EFI_PXE_GUID;

static grub_err_t
clear_txbuffer (struct grub_net_card *dev)
{
  grub_efi_status_t st;
  grub_efi_simple_network_t *net = dev->efi_net;
  grub_uint64_t limit_time = grub_get_time_ms () + 4000;
  void *txbuf;
  int retry = 0;

  if (dev->txbusy)
    while (1)
      {
	txbuf = NULL;
	st = efi_call_3 (net->get_status, net, 0, &txbuf);
	if (st != GRUB_EFI_SUCCESS)
	  return grub_error (GRUB_ERR_IO,
			     N_("couldn't send network packet"));
	/*
	   Some buggy firmware could return an arbitrary address instead of the
	   txbuf address we trasmitted, so just check that txbuf is non NULL
	   for success.  This is ok because we open the SNP protocol in
	   exclusive mode so we know we're the only ones transmitting on this
	   box and since we only transmit one packet at a time we know our
	   transmit was successfull.
	 */
	if (txbuf)
	  {
	    dev->txbusy = 0;
	    break;
	  }
	if (!retry)
	  {
	    st = efi_call_7 (net->transmit, net, 0, dev->last_pkt_size,
			     dev->txbuf, NULL, NULL, NULL);
	    if (st != GRUB_EFI_SUCCESS)
	      return grub_error (GRUB_ERR_IO,
				 N_("couldn't send network packet"));
	    retry = 1;
	  }
	if (limit_time < grub_get_time_ms ())
	  return grub_error (GRUB_ERR_TIMEOUT,
			     N_("couldn't send network packet"));
      }
  return GRUB_ERR_NONE;
}

static grub_err_t
send_card_buffer (struct grub_net_card *dev,
		  struct grub_net_buff *pack)
{
  grub_efi_status_t st;
  grub_efi_simple_network_t *net = dev->efi_net;
  grub_err_t ret;
  void *txbuf;

  ret = clear_txbuffer (dev);
  if (ret != GRUB_ERR_NONE)
    return ret;

  dev->last_pkt_size = (pack->tail - pack->data);
  if (dev->last_pkt_size > dev->mtu)
    dev->last_pkt_size = dev->mtu;

  grub_memcpy (dev->txbuf, pack->data, dev->last_pkt_size);

  st = efi_call_7 (net->transmit, net, 0, dev->last_pkt_size,
		   dev->txbuf, NULL, NULL, NULL);
  if (st != GRUB_EFI_SUCCESS)
    return grub_error (GRUB_ERR_IO, N_("couldn't send network packet"));

  /*
     The card may have sent out the packet immediately - set txbusy
     to 0 in this case.
     Cases were observed where checking txbuf at the next call
     of send_card_buffer() is too late: 0 is returned in txbuf and
     we run in the GRUB_ERR_TIMEOUT case above.
     Perhaps a timeout in the FW has discarded the recycle buffer.
   */
  txbuf = NULL;
  st = efi_call_3 (net->get_status, net, 0, &txbuf);
  dev->txbusy = !(st == GRUB_EFI_SUCCESS && txbuf);

  return GRUB_ERR_NONE;
}

static struct grub_net_buff *
get_card_packet (struct grub_net_card *dev)
{
  grub_efi_simple_network_t *net = dev->efi_net;
  grub_err_t err;
  grub_efi_status_t st;
  grub_efi_uintn_t bufsize = dev->rcvbufsize;
  struct grub_net_buff *nb;
  int i;

  for (i = 0; i < 2; i++)
    {
      if (!dev->rcvbuf)
	dev->rcvbuf = grub_malloc (dev->rcvbufsize);
      if (!dev->rcvbuf)
	return NULL;

      st = efi_call_7 (net->receive, net, NULL, &bufsize,
		       dev->rcvbuf, NULL, NULL, NULL);
      if (st != GRUB_EFI_BUFFER_TOO_SMALL)
	break;
      dev->rcvbufsize = 2 * ALIGN_UP (dev->rcvbufsize > bufsize
				      ? dev->rcvbufsize : bufsize, 64);
      grub_free (dev->rcvbuf);
      dev->rcvbuf = 0;
    }

  if (st != GRUB_EFI_SUCCESS)
    return NULL;

  nb = grub_netbuff_alloc (bufsize + 2);
  if (!nb)
    return NULL;

  /* Reserve 2 bytes so that 2 + 14/18 bytes of ethernet header is divisible
     by 4. So that IP header is aligned on 4 bytes. */
  if (grub_netbuff_reserve (nb, 2))
    {
      grub_netbuff_free (nb);
      return NULL;
    }
  grub_memcpy (nb->data, dev->rcvbuf, bufsize);
  err = grub_netbuff_put (nb, bufsize);
  if (err)
    {
      grub_netbuff_free (nb);
      return NULL;
    }

  return nb;
}

static grub_err_t
open_card (struct grub_net_card *dev)
{
  grub_efi_simple_network_t *net;

  /* Try to reopen SNP exlusively to close any active MNP protocol instance
     that may compete for packet polling
   */
  net = grub_efi_open_protocol (dev->efi_handle, &net_io_guid,
				GRUB_EFI_OPEN_PROTOCOL_BY_EXCLUSIVE);
  if (net)
    {
      if (net->mode->state == GRUB_EFI_NETWORK_STOPPED
	  && efi_call_1 (net->start, net) != GRUB_EFI_SUCCESS)
	return grub_error (GRUB_ERR_NET_NO_CARD, "%s: net start failed",
			   dev->name);

      if (net->mode->state == GRUB_EFI_NETWORK_STOPPED)
	return grub_error (GRUB_ERR_NET_NO_CARD, "%s: card stopped",
			   dev->name);

      if (net->mode->state == GRUB_EFI_NETWORK_STARTED
	  && efi_call_3 (net->initialize, net, 0, 0) != GRUB_EFI_SUCCESS)
	return grub_error (GRUB_ERR_NET_NO_CARD, "%s: net initialize failed",
			   dev->name);

      /* Enable hardware receive filters if driver declares support for it.
	 We need unicast and broadcast and additionaly all nodes and
	 solicited multicast for IPv6. Solicited multicast is per-IPv6
	 address and we currently do not have API to do it so simply
	 enable the all node addresses and the link local address.  We do this
	 because some firmware has been found to not do promiscuous multicast
	 mode properly.

	 This does trust firmware to do what it claims to do.
       */
      if (net->mode->receive_filter_mask)
	{
	  grub_uint32_t filters = GRUB_EFI_SIMPLE_NETWORK_RECEIVE_UNICAST   |
				  GRUB_EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST |
				  GRUB_EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST;
	  grub_efi_status_t st;
	  grub_efi_mac_address_t mac_filter[2] = {
		  { 0x1, 0, 0x5e, 0, 0, 1, },
		  { 0x33, 0x33, 0, 0, 0, 1, },};

	  filters &= net->mode->receive_filter_mask;
	  if (net->mode->max_mcast_filter_count < 2)
	    filters &= ~GRUB_EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST;

	  if (filters & GRUB_EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST)
	    st = efi_call_6 (net->receive_filters, net, filters, 0, 0, 2,
			     mac_filter);
	  else
	    st = efi_call_6 (net->receive_filters, net, filters, 0, 0, 0,
			     NULL);
	  if (st != GRUB_EFI_SUCCESS)
	    grub_dprintf("efinet", "failed to set receive filters %u, %u\n",
			 (unsigned)filters, (unsigned)st);
	}

      efi_call_4 (grub_efi_system_table->boot_services->close_protocol,
		  dev->efi_net, &net_io_guid,
		  grub_efi_image_handle, dev->efi_handle);
      dev->efi_net = net;
    }

  /* If it failed we just try to run as best as we can */
  return GRUB_ERR_NONE;
}

/* We only need the lower 24 bits of the address, so just take the bottom part
   of the address and convert it over.
 */
static void
solicited_node_mcast_addr_to_mac (grub_uint64_t addr,
				  grub_efi_mac_address_t mac)
{
  grub_uint64_t cpu_addr = grub_be_to_cpu64(addr);
  int i, c = 0;

  /* The format is 33:33:xx:xx:xx:xx, where xx is the last 32 bits of the
     multicast address.

     The solicited node mcast addr is in the format ff02:0:0:0:0:1:ffxx:xxxx,
     where xx is the last 24 bits of the ipv6 address.
   */
  mac[0] = 0x33;
  mac[1] = 0x33;
  mac[2] = 0xff;
  for (i = 3; i < 6; i++, c++)
    mac[i] = (grub_uint8_t)((cpu_addr >> (16 - 8 * c)) & 0xff);
}

static void
add_addr (struct grub_net_card *dev,
	  const grub_net_network_level_address_t *address)
{
  grub_efi_simple_network_t *net = dev->efi_net;
  grub_efi_mac_address_t mac_filters[16];
  grub_uint32_t current_settings = net->mode->receive_filter_setting;
  grub_efi_status_t st;
  unsigned slot = net->mode->mcast_filter_count;

  /* We don't need to add anything for ipv4 addresses. */
  if (address->type != GRUB_NET_NETWORK_LEVEL_PROTOCOL_IPV6)
    return;

  if ((slot >= net->mode->max_mcast_filter_count)
      || !(GRUB_EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST &
	   net->mode->receive_filter_mask))
    return;

  /* Copy the existing filters and add the new filter. */
  grub_memcpy(mac_filters, net->mode->mcast_filter,
	      sizeof (grub_efi_mac_address_t) * slot);
  solicited_node_mcast_addr_to_mac (address->ipv6[1], mac_filters[slot++]);

  /* Some firmware will hang if we try to modify the receive filters while the
     tx buffer still has something in the queue, so clear it before resetting
     the filters. */
  if (clear_txbuffer (dev) != GRUB_ERR_NONE)
    grub_dprintf("efinet", "couldn't clear the txbuffer.\n");

  st = efi_call_6 (net->receive_filters, net, current_settings, 0, 0, slot,
		   mac_filters);
  if (st != GRUB_EFI_SUCCESS)
    grub_dprintf("efinet", "failed to add new receive filter %u\n",
		 (unsigned)st);
}

static void
close_card (struct grub_net_card *dev)
{
  efi_call_1 (dev->efi_net->shutdown, dev->efi_net);
  efi_call_1 (dev->efi_net->stop, dev->efi_net);
  efi_call_4 (grub_efi_system_table->boot_services->close_protocol,
	      dev->efi_net, &net_io_guid,
	      grub_efi_image_handle, dev->efi_handle);
}

static struct grub_net_card_driver efidriver =
  {
    .name = "efinet",
    .open = open_card,
    .close = close_card,
    .send = send_card_buffer,
    .recv = get_card_packet,
    .add_addr = add_addr,
  };

grub_efi_handle_t
grub_efinet_get_device_handle (struct grub_net_card *card)
{
  if (!card || card->driver != &efidriver)
    return 0;
  return card->efi_handle;
}

static int
grub_efinet_is_mac_device (struct grub_net_card *card)
{
  grub_efi_handle_t efi_handle;
  grub_efi_device_path_t *dp;
  grub_efi_device_path_t *next, *p;
  grub_efi_uint8_t type;
  grub_efi_uint8_t subtype;

  efi_handle = grub_efinet_get_device_handle (card);

  if (!efi_handle)
    return 0;

  dp = grub_efi_get_device_path (efi_handle);

  if (GRUB_EFI_END_ENTIRE_DEVICE_PATH (dp))
    return 0;

  for (p = (grub_efi_device_path_t *) dp, next = GRUB_EFI_NEXT_DEVICE_PATH (p);
       ! GRUB_EFI_END_ENTIRE_DEVICE_PATH (next);
       p = next, next = GRUB_EFI_NEXT_DEVICE_PATH (next))
    ;

  if (p)
    {
      type = GRUB_EFI_DEVICE_PATH_TYPE (p);
      subtype = GRUB_EFI_DEVICE_PATH_SUBTYPE (p);

      if (type == GRUB_EFI_MESSAGING_DEVICE_PATH_TYPE
	  && subtype == GRUB_EFI_MAC_ADDRESS_DEVICE_PATH_SUBTYPE)
	{
	  return 1;
	}
    }

  return 0;
}

static void
grub_efinet_findcards (void)
{
  grub_efi_uintn_t num_handles;
  grub_efi_handle_t *handles;
  grub_efi_handle_t *handle;
  int i = 0;

  /* Find handles which support the disk io interface.  */
  handles = grub_efi_locate_handle (GRUB_EFI_BY_PROTOCOL, &net_io_guid,
				    0, &num_handles);
  if (! handles)
    return;
  for (handle = handles; num_handles--; handle++)
    {
      grub_efi_simple_network_t *net;
      struct grub_net_card *card;
      grub_efi_device_path_t *dp, *parent = NULL, *child = NULL;

      /* EDK2 UEFI PXE driver creates IPv4 and IPv6 messaging devices as
	 children of main MAC messaging device. We only need one device with
	 bound SNP per physical card, otherwise they compete with each other
	 when polling for incoming packets.
       */
      dp = grub_efi_get_device_path (*handle);
      if (!dp)
	continue;
      for (; ! GRUB_EFI_END_ENTIRE_DEVICE_PATH (dp); dp = GRUB_EFI_NEXT_DEVICE_PATH (dp))
	{
	  parent = child;
	  child = dp;
	}
      if (child
	  && GRUB_EFI_DEVICE_PATH_TYPE (child) == GRUB_EFI_MESSAGING_DEVICE_PATH_TYPE
	  && (GRUB_EFI_DEVICE_PATH_SUBTYPE (child) == GRUB_EFI_IPV4_DEVICE_PATH_SUBTYPE
	      || GRUB_EFI_DEVICE_PATH_SUBTYPE (child) == GRUB_EFI_IPV6_DEVICE_PATH_SUBTYPE)
	  && parent
	  && GRUB_EFI_DEVICE_PATH_TYPE (parent) == GRUB_EFI_MESSAGING_DEVICE_PATH_TYPE
	  && GRUB_EFI_DEVICE_PATH_SUBTYPE (parent) == GRUB_EFI_MAC_ADDRESS_DEVICE_PATH_SUBTYPE)
	continue;

      net = grub_efi_open_protocol (*handle, &net_io_guid,
				    GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
      if (! net)
	/* This should not happen... Why?  */
	continue;

      if (net->mode->state == GRUB_EFI_NETWORK_STOPPED
	  && efi_call_1 (net->start, net) != GRUB_EFI_SUCCESS)
	continue;

      if (net->mode->state == GRUB_EFI_NETWORK_STOPPED)
	continue;

      if (net->mode->state == GRUB_EFI_NETWORK_STARTED
	  && efi_call_3 (net->initialize, net, 0, 0) != GRUB_EFI_SUCCESS)
	continue;

      card = grub_zalloc (sizeof (struct grub_net_card));
      if (!card)
	{
	  grub_print_error ();
	  grub_free (handles);
	  return;
	}

      card->mtu = net->mode->max_packet_size;
      card->txbufsize = ALIGN_UP (card->mtu, 64) + 256;
      card->txbuf = grub_zalloc (card->txbufsize);
      if (!card->txbuf)
	{
	  grub_print_error ();
	  grub_free (handles);
	  grub_free (card);
	  return;
	}
      card->txbusy = 0;

      card->rcvbufsize = ALIGN_UP (card->mtu, 64) + 256;

      card->name = grub_xasprintf ("efinet%d", i++);
      card->driver = &efidriver;
      card->flags = 0;
      card->default_address.type = GRUB_NET_LINK_LEVEL_PROTOCOL_ETHERNET;
      grub_memcpy (card->default_address.mac,
		   net->mode->current_address,
		   sizeof (card->default_address.mac));
      card->efi_net = net;
      card->efi_handle = *handle;
      card->is_efi_mac_device = grub_efinet_is_mac_device;

      grub_net_card_register (card);
    }
  grub_free (handles);
}

static void
grub_efi_net_config_real (grub_efi_handle_t hnd, char **device,
			  char **path)
{
  struct grub_net_card *card;
  grub_efi_device_path_t *dp;

  grub_printf("DEBUG: efinet initializing!\n");

  dp = grub_efi_get_device_path (hnd);
  if (! dp)
    return;

  FOR_NET_CARDS (card)
  {
    grub_efi_device_path_t *cdp;
    struct grub_efi_pxe *pxe;
    struct grub_efi_pxe_mode *pxe_mode;
    if (card->driver != &efidriver)
      continue;
    cdp = grub_efi_get_device_path (card->efi_handle);
    if (! cdp) {
      grub_printf("DEBUG: !cdp\n");
      continue;
    }
    if (grub_efi_compare_device_paths (dp, cdp) != 0)
      {
	grub_efi_device_path_t *ldp, *dup_dp, *dup_ldp;
	int match;

	/* EDK2 UEFI PXE driver creates pseudo devices with type IPv4/IPv6
	   as children of Ethernet card and binds PXE and Load File protocols
	   to it. Loaded Image Device Path protocol will point to these pseudo
	   devices. We skip them when enumerating cards, so here we need to
	   find matching MAC device.
         */
	ldp = grub_efi_find_last_device_path (dp);
	if (GRUB_EFI_DEVICE_PATH_TYPE (ldp) != GRUB_EFI_MESSAGING_DEVICE_PATH_TYPE
	    || (GRUB_EFI_DEVICE_PATH_SUBTYPE (ldp) != GRUB_EFI_IPV4_DEVICE_PATH_SUBTYPE
		&& GRUB_EFI_DEVICE_PATH_SUBTYPE (ldp) != GRUB_EFI_IPV6_DEVICE_PATH_SUBTYPE)) {
      grub_printf("DEBUG: first ifbunch continue\n");
	  continue;
    }
	dup_dp = grub_efi_duplicate_device_path (dp);
	if (!dup_dp) {
      grub_printf("DEBUG: second if continue\n");
	  continue;
    }
	dup_ldp = grub_efi_find_last_device_path (dup_dp);
	dup_ldp->type = GRUB_EFI_END_DEVICE_PATH_TYPE;
	dup_ldp->subtype = GRUB_EFI_END_ENTIRE_DEVICE_PATH_SUBTYPE;
	dup_ldp->length = sizeof (*dup_ldp);
	match = grub_efi_compare_device_paths (dup_dp, cdp) == 0;
	grub_free (dup_dp);
	if (!match) {
      grub_printf("DEBUG: !match\n");
	  continue;
     }
    }
    pxe = grub_efi_open_protocol (hnd, &pxe_io_guid,
				  GRUB_EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (! pxe) {
      grub_printf("DEBUG: Y U NO PXE\n", pxe);
      continue;
    }
    pxe_mode = pxe->mode;

    if (pxe_mode->using_ipv6)
      {
        grub_printf("DEBUG: configure by dhcpv6 reply\n");
	grub_net_configure_by_dhcpv6_reply (card->name, card, 0,
					    (struct grub_net_dhcpv6_packet *)
					    &pxe_mode->dhcp_ack,
					    sizeof (pxe_mode->dhcp_ack),
					    1, device, path);
	if (grub_errno)
	  grub_print_error ();
      }
    else
      {
	grub_net_configure_by_dhcp_ack (card->name, card, 0,
					(struct grub_net_bootp_packet *)
					&pxe_mode->dhcp_ack,
					sizeof (pxe_mode->dhcp_ack),
					1, device, path);
      }
    return;
  }
}

GRUB_MOD_INIT(efinet)
{
  grub_efinet_findcards ();
  grub_efi_net_config = grub_efi_net_config_real;
}

GRUB_MOD_FINI(efinet)
{
  struct grub_net_card *card, *next;

  FOR_NET_CARDS_SAFE (card, next) 
    if (card->driver == &efidriver)
      grub_net_card_unregister (card);
}


/* The LibVMI Library is an introspection library that simplifies access to
* memory in a target virtual machine or in a file containing a dump of
* a system's physical memory.  LibVMI is based on the XenAccess Library.
*
* Author: Kevin Mayer (kevin.mayer@gdata.de)
*
* This file is part of LibVMI.
*
* LibVMI is free software: you can redistribute it and/or modify it under
* the terms of the GNU Lesser General Public License as published by the
* Free Software Foundation, either version 3 of the License, or (at your
* option) any later version.
*
* LibVMI is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
* License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with LibVMI.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "private.h"
#include "driver/xen/xen.h"
#include "driver/xen/xen_private.h"

status_t xen_altp2m_init (vmi_instance_t vmi, uint64_t *init_memsize)
{
    int rc;
    xen_instance_t *xen = xen_get_instance(vmi);
    xc_interface * xch = xen_get_xchandle(vmi);
    domid_t domain_id = xen_get_domainid(vmi);

    xc_dominfo_t info = { 0 };
    if ( 1 == xen->libxcw.xc_domain_getinfo(xch, domain_id, 1, &info) && info.domid == domain_id)
        *init_memsize = info.max_memkb;
    else
        *init_memsize = 0;

    rc = xen->libxcw.xc_domain_setmaxmem(xch, domain_id, ~0);
    if (rc < 0)
        return VMI_FAILURE;

    return VMI_SUCCESS;
}

status_t xen_altp2m_deinit (vmi_instance_t vmi, uint64_t init_memsize)
{
    xen_instance_t *xen = xen_get_instance(vmi);
    xc_interface * xch = xen_get_xchandle(vmi);
    domid_t domain_id = xen_get_domainid(vmi);

    xen->libxcw.xc_domain_setmaxmem(xch, domain_id, init_memsize);

    return VMI_SUCCESS;
}

status_t xen_altp2m_get_domain_state (vmi_instance_t vmi, bool *state)
{
    int rc;
    xen_instance_t *xen = xen_get_instance(vmi);
    xc_interface * xch = xen_get_xchandle(vmi);
    domid_t domain_id = xen_get_domainid(vmi);
    if ( !xch ) {
        errprint("%s error: invalid xc_interface handle\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    if ( domain_id == (domid_t)VMI_INVALID_DOMID ) {
        errprint ("%s error: invalid domid\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    rc = xen->libxcw.xc_altp2m_get_domain_state (xch, domain_id, state);
    if ( rc ) {
        errprint ("xc_altp2m_get_domain_state returned rc: %i\n", rc);
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;

}

status_t xen_altp2m_set_domain_state (vmi_instance_t vmi, bool state)
{
    int rc;
    xen_instance_t *xen = xen_get_instance(vmi);
    xc_interface * xch = xen_get_xchandle(vmi);
    domid_t domain_id = xen_get_domainid(vmi);
    if ( !xch ) {
        errprint("%s error: invalid xc_interface handle\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    if ( domain_id == (domid_t)VMI_INVALID_DOMID ) {
        errprint ("%s error: invalid domid\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    rc = xen->libxcw.xc_altp2m_set_domain_state (xch, domain_id, state);
    if ( rc ) {
        errprint ("xc_altp2m_set_domain_state returned rc: %i\n", rc);
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;

}

uint64_t xen_altp2m_get_max_gpfn(vmi_instance_t vmi)
{
    xen_instance_t *xen = xen_get_instance(vmi);
    return xen->max_gpfn;
}

status_t xen_altp2m_create_physical_page(vmi_instance_t vmi, uint64_t *page_addr)
{
    int rc;
    xen_instance_t *xen = xen_get_instance(vmi);
    xc_interface * xch = xen_get_xchandle(vmi);
    domid_t domain_id = xen_get_domainid(vmi);

    rc = xen->libxcw.xc_domain_populate_physmap_exact(xch, domain_id, 1, 0, 0, page_addr);
    if(rc < 0)
        return VMI_FAILURE;

    return VMI_SUCCESS;
}

status_t xen_altp2m_destroy_physical_page(vmi_instance_t vmi, uint64_t *page_addr)
{
    xen_instance_t *xen = xen_get_instance(vmi);
    xc_interface * xch = xen_get_xchandle(vmi);
    domid_t domain_id = xen_get_domainid(vmi);

    xen->libxcw.xc_domain_decrease_reservation_exact(xch, domain_id, 1, 0, page_addr);

    return VMI_SUCCESS;
}

status_t xen_altp2m_create_p2m ( vmi_instance_t vmi, uint16_t *altp2m_idx )
{
    int rc;
    xen_instance_t *xen = xen_get_instance(vmi);
    xc_interface * xch = xen_get_xchandle(vmi);
    domid_t domain_id = xen_get_domainid(vmi);
    if ( !xch ) {
        errprint("%s error: invalid xc_interface handle\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    if ( domain_id == (domid_t)VMI_INVALID_DOMID ) {
        errprint ("%s error: invalid domid\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    rc = xen->libxcw.xc_altp2m_create_view (xch, domain_id, VMI_MEMACCESS_N, altp2m_idx );
    if ( rc ) {
        errprint ("xc_altp2m_create_view returned rc: %i\n", rc);
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;

}

status_t xen_altp2m_destroy_p2m ( vmi_instance_t vmi, uint16_t altp2m_idx )
{
    int rc;
    xen_instance_t *xen = xen_get_instance(vmi);
    xc_interface * xch = xen_get_xchandle(vmi);
    domid_t domain_id = xen_get_domainid(vmi);

    if ( !xch ) {
        errprint("%s error: invalid xc_interface handle\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    if ( domain_id == (domid_t)VMI_INVALID_DOMID ) {
        errprint ("%s error: invalid domid\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    rc = xen->libxcw.xc_altp2m_destroy_view (xch, domain_id, altp2m_idx );
    if ( rc ) {
        errprint ("xc_altp2m_destroy_view returned rc: %i\n", rc);
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;

}

status_t xen_altp2m_switch_p2m ( vmi_instance_t vmi, uint16_t altp2m_idx )
{
    int rc;
    xen_instance_t *xen = xen_get_instance(vmi);
    xc_interface * xch = xen_get_xchandle(vmi);
    domid_t domain_id = xen_get_domainid(vmi);
    if ( !xch ) {
        errprint("%s error: invalid xc_interface handle\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    if ( domain_id == (domid_t)VMI_INVALID_DOMID ) {
        errprint ("%s error: invalid domid\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    rc = xen->libxcw.xc_altp2m_switch_to_view (xch, domain_id, altp2m_idx );
    if ( rc ) {
        errprint ("xc_altp2m_switch_to_view returned rc: %i\n", rc);
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;
}

status_t xen_altp2m_change_gfn ( vmi_instance_t vmi, uint16_t altp2m_idx, addr_t old_gfn, addr_t new_gfn )
{
    int rc;
    xen_instance_t *xen = xen_get_instance(vmi);
    xc_interface * xch = xen_get_xchandle(vmi);
    domid_t domain_id = xen_get_domainid(vmi);
    if ( !xch ) {
        errprint("%s error: invalid xc_interface handle\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    if ( domain_id == (domid_t)VMI_INVALID_DOMID ) {
        errprint ("%s error: invalid domid\n", __FUNCTION__);
        return VMI_FAILURE;
    }
    rc = xen->libxcw.xc_altp2m_change_gfn (xch, domain_id, altp2m_idx, old_gfn, new_gfn );
    if ( rc ) {
        errprint ("xc_altp2m_change_gfn returned rc: %i\n", rc);
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;
}

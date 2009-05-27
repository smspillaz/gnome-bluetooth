/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2005-2008  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2006-2007  Bastien Nocera <hadess@hadess.net>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <dbus/dbus-glib.h>

#include <bluetooth-client.h>
#include <bluetooth-killswitch.h>
#include <bluetooth-chooser.h>

#include "adapter.h"
#include "general.h"

static BluetoothClient *client;
static GtkTreeModel *adapter_model;
static BluetoothKillswitch *killswitch;

#define KILLSWITCH_PAGE_NUM(n) (gtk_notebook_get_n_pages (n) - 2)
#define NO_ADAPTERS_PAGE_NUM(n) (-1)

typedef struct adapter_data adapter_data;
struct adapter_data {
	DBusGProxy *proxy;
	GtkWidget *notebook;
	GtkWidget *child;
	GtkWidget *button_discoverable;
	GtkWidget *name_vbox, *devices_table;
	GtkWidget *entry;
	GtkWidget *button_delete;
	GtkWidget *button_disconnect;
	GtkWidget *chooser;
	GtkTreeRowReference *reference;
	guint signal_discoverable;
	gboolean powered;
	gboolean discoverable;
	gboolean is_default;
	guint timeout_value;
	int name_changed;
};

static void update_visibility(adapter_data *adapter);

static void block_signals(adapter_data *adapter)
{
	g_signal_handler_block(adapter->button_discoverable,
						adapter->signal_discoverable);
}

static void unblock_signals(adapter_data *adapter)
{
	g_signal_handler_unblock(adapter->button_discoverable,
						adapter->signal_discoverable);
}

static void discoverable_changed_cb(GtkWidget *button, gpointer user_data)
{
	adapter_data *adapter = user_data;
	GValue discoverable = { 0 };
	GValue timeout = { 0 };

	g_value_init(&discoverable, G_TYPE_BOOLEAN);
	g_value_init(&timeout, G_TYPE_UINT);

	gtk_toggle_button_set_inconsistent (GTK_TOGGLE_BUTTON (button), FALSE);
	g_value_set_boolean(&discoverable, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button)));
	g_value_set_uint(&timeout, 0);

	dbus_g_proxy_call(adapter->proxy, "SetProperty", NULL,
					G_TYPE_STRING, "Discoverable",
					G_TYPE_VALUE, &discoverable,
					G_TYPE_INVALID, G_TYPE_INVALID);

	dbus_g_proxy_call(adapter->proxy, "SetProperty", NULL,
					G_TYPE_STRING, "DiscoverableTimeout",
					G_TYPE_VALUE, &timeout,
					G_TYPE_INVALID, G_TYPE_INVALID);

	g_value_unset(&discoverable);
	g_value_unset(&timeout);
}

static void name_callback(GtkWidget *editable, gpointer user_data)
{
	adapter_data *adapter = user_data;

	adapter->name_changed = 1;
}

static gboolean focus_callback(GtkWidget *editable,
				GdkEventFocus *event, gpointer user_data)
{
	adapter_data *adapter = user_data;
	GValue value = { 0 };
	const gchar *text;

	if (!adapter->name_changed)
		return FALSE;

	g_value_init(&value, G_TYPE_STRING);

	text = gtk_entry_get_text(GTK_ENTRY(editable));

	g_value_set_string(&value, text);

	dbus_g_proxy_call(adapter->proxy, "SetProperty", NULL,
			G_TYPE_STRING, "Name",
			G_TYPE_VALUE, &value, G_TYPE_INVALID, G_TYPE_INVALID);

	g_value_unset(&value);

	adapter->name_changed = 0;

	return FALSE;
}

static void update_buttons(adapter_data *adapter, gboolean selected, gboolean connected)
{
	gtk_widget_set_sensitive(adapter->button_delete, selected);
	gtk_widget_set_sensitive(adapter->button_disconnect, connected);
}

static void
device_selected_cb(GObject *object,
		   GParamSpec *spec,
		   adapter_data *adapter)
{
	char *address;
	gboolean connected;

	g_object_get (G_OBJECT (adapter->chooser),
		      "device-selected", &address,
		      "device-selected-is-connected", &connected,
		      NULL);
	update_buttons(adapter, (address != NULL), connected);
}

static void wizard_callback(GtkWidget *button, gpointer user_data)
{
	const char *command = "bluetooth-wizard";

	if (!g_spawn_command_line_async(command, NULL))
		g_printerr("Couldn't execute command: %s\n", command);
}

static gboolean show_confirm_dialog(void)
{
	GtkWidget *dialog;
	gint response;
	gchar *text;

	text = g_strdup_printf("<big><b>%s</b></big>\n\n%s",
				_("Remove from list of known devices?"),
				_("If you delete the device, you have to "
					"set it up again before next use."));
	dialog = gtk_message_dialog_new_with_markup(NULL, GTK_DIALOG_MODAL,
				GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, NULL);
	gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog), text);
	g_free(text);

	gtk_dialog_add_button(GTK_DIALOG(dialog), GTK_STOCK_CANCEL,
							GTK_RESPONSE_CANCEL);
	gtk_dialog_add_button(GTK_DIALOG(dialog), GTK_STOCK_DELETE,
							GTK_RESPONSE_ACCEPT);

	response = gtk_dialog_run(GTK_DIALOG(dialog));

	gtk_widget_destroy(dialog);

	if (response == GTK_RESPONSE_ACCEPT)
		return TRUE;

	return FALSE;
}

static void delete_callback(GtkWidget *button, gpointer user_data)
{
	adapter_data *adapter = user_data;
	DBusGProxy *device;
	const char *path;

	g_object_get (G_OBJECT (adapter->chooser), "device-selected-proxy", &device, NULL);

	if (device == NULL)
		return;

	path = dbus_g_proxy_get_path(device);

	if (show_confirm_dialog() == TRUE) {
		dbus_g_proxy_call(adapter->proxy, "RemoveDevice", NULL,
					DBUS_TYPE_G_OBJECT_PATH, path,
					G_TYPE_INVALID, G_TYPE_INVALID);
	}

	g_object_unref(device);
}

static void disconnect_callback(GtkWidget *button, gpointer user_data)
{
	adapter_data *adapter = user_data;
	DBusGProxy *device;

	g_object_get (G_OBJECT (adapter->chooser), "device-selected-proxy", &device, NULL);

	if (device == NULL)
		return;

	dbus_g_proxy_call(device, "Disconnect", NULL,
					G_TYPE_INVALID, G_TYPE_INVALID);

	g_object_unref(device);

	gtk_widget_set_sensitive(button, FALSE);
}

static void
set_current_page (GtkNotebook *notebook)
{
	if (gtk_tree_model_iter_n_children (adapter_model, NULL) == 0) {
		if (bluetooth_killswitch_has_killswitches (killswitch) != FALSE) {
			gtk_notebook_set_current_page (notebook,
						       KILLSWITCH_PAGE_NUM(notebook));
		} else {
			gtk_notebook_set_current_page (notebook,
						       NO_ADAPTERS_PAGE_NUM(notebook));
		}
	}
}

static void create_adapter(adapter_data *adapter)
{
	GHashTable *hash = NULL;
	GValue *value;
	DBusGProxy *default_proxy;
	const gchar *address, *name;
	gboolean powered, discoverable;
	guint timeout;

	GtkWidget *mainbox;
	GtkWidget *vbox;
	GtkWidget *alignment;
	GtkWidget *table;
	GtkWidget *label;
	GtkWidget *image;
	GtkWidget *button;
	GtkWidget *entry;
	GtkWidget *buttonbox;
	int page_num;

	dbus_g_proxy_call(adapter->proxy, "GetProperties", NULL, G_TYPE_INVALID,
				dbus_g_type_get_map("GHashTable",
						G_TYPE_STRING, G_TYPE_VALUE),
				&hash, G_TYPE_INVALID);

	if (hash != NULL) {
		value = g_hash_table_lookup(hash, "Address");
		address = value ? g_value_get_string(value) : NULL;

		value = g_hash_table_lookup(hash, "Name");
		name = value ? g_value_get_string(value) : NULL;

		value = g_hash_table_lookup(hash, "Powered");
		powered = value ? g_value_get_boolean(value) : FALSE;

		value = g_hash_table_lookup(hash, "Discoverable");
		discoverable = value ? g_value_get_boolean(value) : FALSE;

		value = g_hash_table_lookup(hash, "DiscoverableTimeout");
		timeout = value ? g_value_get_uint(value) : 0;
	} else {
		address = NULL;
		name = NULL;
		powered = FALSE;
		discoverable = FALSE;
		timeout = 0;
	}

	adapter->powered = powered;
	adapter->discoverable = discoverable;
	adapter->timeout_value = timeout;

	default_proxy = bluetooth_client_get_default_adapter (client);
	if (default_proxy != NULL) {
		adapter->is_default = g_str_equal (dbus_g_proxy_get_path (default_proxy),
						   dbus_g_proxy_get_path (adapter->proxy));
		g_object_unref (default_proxy);
	}

	mainbox = gtk_vbox_new(FALSE, 6);
	gtk_container_set_border_width(GTK_CONTAINER(mainbox), 12);

	page_num = gtk_notebook_prepend_page(GTK_NOTEBOOK(adapter->notebook),
							mainbox, NULL);

	adapter->child = mainbox;

	vbox = gtk_vbox_new(FALSE, 6);
	gtk_box_pack_start(GTK_BOX(mainbox), vbox, FALSE, TRUE, 0);

	/* The discoverable checkbox */
	button = gtk_check_button_new_with_mnemonic (_("_Discoverable"));
	if (powered == FALSE)
		discoverable = FALSE;
	if (discoverable != FALSE && timeout == 0)
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
	else if (discoverable == FALSE)
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), FALSE);
	else {
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
		gtk_toggle_button_set_inconsistent (GTK_TOGGLE_BUTTON (button), TRUE);
	}
	gtk_widget_set_sensitive (button, adapter->powered);

	adapter->button_discoverable = button;
	adapter->signal_discoverable = g_signal_connect(G_OBJECT(button), "toggled",
							G_CALLBACK(discoverable_changed_cb), adapter);

	gtk_box_pack_start(GTK_BOX(vbox), button, FALSE, FALSE, 0);

	/* The friendly name */
	vbox = gtk_vbox_new(FALSE, 6);
	gtk_box_pack_start(GTK_BOX(mainbox), vbox, FALSE, FALSE, 0);

	label = create_label(_("Friendly name"));
	gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

	alignment = gtk_alignment_new (0.5, 0.5, 1, 1);
	gtk_widget_show (alignment);
	gtk_box_pack_start (GTK_BOX (vbox), alignment, TRUE, TRUE, 0);
	gtk_alignment_set_padding (GTK_ALIGNMENT (alignment), 0, 0, 12, 0);

	entry = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(entry), 248);
	gtk_widget_set_size_request(entry, 240, -1);
	gtk_container_add (GTK_CONTAINER (alignment), entry);

	if (name != NULL)
		gtk_entry_set_text(GTK_ENTRY(entry), name);

	adapter->entry = entry;
	adapter->name_vbox = vbox;

	g_signal_connect(G_OBJECT(entry), "changed",
					G_CALLBACK(name_callback), adapter);
	g_signal_connect(G_OBJECT(entry), "focus-out-event",
					G_CALLBACK(focus_callback), adapter);

	gtk_widget_set_sensitive (adapter->name_vbox, adapter->powered);

	/* The known devices */
	table = gtk_table_new(2, 2, FALSE);
	gtk_box_pack_start(GTK_BOX(mainbox), table, TRUE, TRUE, 0);

	label = create_label(_("Known devices"));
	gtk_table_attach(GTK_TABLE(table), label, 0, 2, 0, 1,
			 GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 6);

	/* Note that this will only ever show the devices on the default
	 * adapter, this is on purpose */
	adapter->chooser = bluetooth_chooser_new (NULL);
	g_object_set (adapter->chooser,
		      "show-search", FALSE,
		      "show-device-type", FALSE,
		      "show-device-category", FALSE,
		      "show-pairing", TRUE,
		      "show-connected", TRUE,
		      "device-category-filter", BLUETOOTH_CATEGORY_PAIRED_OR_TRUSTED,
		      NULL);

	g_signal_connect (adapter->chooser, "notify::device-selected",
			  G_CALLBACK(device_selected_cb), adapter);
	g_signal_connect (adapter->chooser, "notify::device-selected-is-connected",
			  G_CALLBACK(device_selected_cb), adapter);

	gtk_table_attach(GTK_TABLE(table), adapter->chooser, 0, 1, 1, 2,
			 GTK_EXPAND | GTK_FILL, GTK_EXPAND | GTK_FILL, 0, 0);

	adapter->devices_table = table;

	buttonbox = gtk_vbutton_box_new();
	gtk_button_box_set_layout(GTK_BUTTON_BOX(buttonbox),
						GTK_BUTTONBOX_START);
	gtk_box_set_spacing(GTK_BOX(buttonbox), 6);
	gtk_box_set_homogeneous(GTK_BOX(buttonbox), FALSE);
	gtk_table_attach(GTK_TABLE(table), buttonbox, 1, 2, 1, 2,
			 GTK_FILL, GTK_FILL, 6, 6);

	button = gtk_button_new_with_label(_("Setup new device..."));
	image = gtk_image_new_from_stock(GTK_STOCK_ADD,
						GTK_ICON_SIZE_BUTTON);
	gtk_button_set_image(GTK_BUTTON(button), image);
	gtk_box_pack_start(GTK_BOX(buttonbox), button, FALSE, FALSE, 0);

	g_signal_connect(G_OBJECT(button), "clicked",
				G_CALLBACK(wizard_callback), adapter);

	button = gtk_button_new_with_label(_("Disconnect"));
	image = gtk_image_new_from_stock(GTK_STOCK_DISCONNECT,
						GTK_ICON_SIZE_BUTTON);
	gtk_button_set_image(GTK_BUTTON(button), image);
	gtk_box_pack_end(GTK_BOX(buttonbox), button, FALSE, FALSE, 0);
	gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(buttonbox),
								button, TRUE);
	gtk_widget_set_sensitive(button, FALSE);

	g_signal_connect(G_OBJECT(button), "clicked",
				G_CALLBACK(disconnect_callback), adapter);

	adapter->button_disconnect = button;

	button = gtk_button_new_with_label(_("Delete"));
	image = gtk_image_new_from_stock(GTK_STOCK_DELETE,
						GTK_ICON_SIZE_BUTTON);
	gtk_button_set_image(GTK_BUTTON(button), image);
	gtk_box_pack_end(GTK_BOX(buttonbox), button, FALSE, FALSE, 0);
	gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(buttonbox),
								button, TRUE);
	gtk_widget_set_sensitive(button, FALSE);

	g_signal_connect(G_OBJECT(button), "clicked",
				G_CALLBACK(delete_callback), adapter);

	adapter->button_delete = button;

	gtk_widget_set_sensitive (adapter->devices_table, adapter->powered);

	g_object_set_data(G_OBJECT(mainbox), "adapter", adapter);

	gtk_widget_show_all(mainbox);

	if (adapter->is_default != FALSE)
		gtk_notebook_set_current_page (GTK_NOTEBOOK (adapter->notebook), page_num);
}

static void update_visibility(adapter_data *adapter)
{
	gboolean inconsistent, enabled;
	block_signals(adapter);

	/* Switch off a few widgets */
	gtk_widget_set_sensitive (adapter->button_discoverable, adapter->powered);
	gtk_widget_set_sensitive (adapter->devices_table, adapter->powered);
	gtk_widget_set_sensitive (adapter->name_vbox, adapter->powered);

	if (adapter->discoverable != FALSE && adapter->timeout_value == 0) {
		inconsistent = FALSE;
		enabled = TRUE;
	} else if (adapter->discoverable == FALSE) {
		inconsistent = enabled = FALSE;
	} else {
		inconsistent = enabled = TRUE;
	}

	gtk_toggle_button_set_inconsistent (GTK_TOGGLE_BUTTON (adapter->button_discoverable), inconsistent);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (adapter->button_discoverable), enabled);

	unblock_signals(adapter);
}

static void property_changed(DBusGProxy *proxy, const char *property,
					GValue *value, gpointer user_data)
{
	adapter_data *adapter = user_data;

	if (g_str_equal(property, "Powered") == TRUE) {
		gboolean powered = g_value_get_boolean(value);

		adapter->powered = powered;

		if (powered == FALSE)
			adapter->discoverable = FALSE;

		update_visibility(adapter);
	} else if (g_str_equal(property, "Discoverable") == TRUE) {
		gboolean discoverable = g_value_get_boolean(value);

		adapter->powered = TRUE;
		adapter->discoverable = discoverable;

		update_visibility(adapter);
	} else if (g_str_equal(property, "DiscoverableTimeout") == TRUE) {
		guint timeout = g_value_get_uint(value);

		adapter->timeout_value = timeout;

		update_visibility(adapter);
	}
}

static adapter_data *adapter_alloc(GtkTreeModel *model,
		GtkTreePath *path, GtkTreeIter *iter, gpointer user_data)
{
	DBusGProxy *proxy;
	adapter_data *adapter;

	gtk_tree_model_get(model, iter,
			   BLUETOOTH_COLUMN_PROXY, &proxy,
			   -1);

	if (proxy == NULL)
		return NULL;

	adapter = g_new0(adapter_data, 1);
	adapter->notebook = user_data;
	adapter->reference = gtk_tree_row_reference_new(model, path);
	adapter->proxy = proxy;

	return adapter;
}

static gboolean adapter_create(gpointer user_data)
{
	adapter_data *adapter = user_data;

	dbus_g_proxy_connect_signal(adapter->proxy, "PropertyChanged",
				G_CALLBACK(property_changed), adapter, NULL);

	create_adapter(adapter);

	return FALSE;
}

static gboolean adapter_insert(GtkTreeModel *model, GtkTreePath *path,
					GtkTreeIter *iter, gpointer user_data)
{
	adapter_data *adapter;

	adapter = adapter_alloc(model, path, iter, user_data);
	if (adapter == NULL)
		return FALSE;

	adapter_create(adapter);

	return FALSE;
}

static void adapter_added(GtkTreeModel *model, GtkTreePath *path,
					GtkTreeIter *iter, gpointer user_data)
{
	adapter_data *adapter;

	adapter = adapter_alloc(model, path, iter, user_data);
	if (adapter == NULL)
		return;

	/* XXX This is needed so that we can run dbus_g_proxy_add_signal()
	 * for "PropertyChanged" on the adapter, remove when we have some
	 * decent D-Bus bindings */
	g_idle_add(adapter_create, adapter);
}

static void adapter_removed(GtkTreeModel *model, GtkTreePath *path,
							gpointer user_data)
{
	GtkNotebook *notebook = user_data;
	int i, count = gtk_notebook_get_n_pages(notebook);

	for (i = 0; i < count; i++) {
		GtkWidget *widget;
		adapter_data *adapter;

		widget = gtk_notebook_get_nth_page(notebook, i);
		if (widget == NULL)
			continue;

		adapter = g_object_get_data(G_OBJECT(widget), "adapter");
		if (adapter == NULL)
			continue;

		if (gtk_tree_row_reference_valid(adapter->reference) == TRUE)
			continue;

		gtk_tree_row_reference_free(adapter->reference);
		adapter->reference = NULL;

		gtk_notebook_remove_page(notebook, i);
		set_current_page (notebook);
		/* When the default adapter changes, the adapter_changed signal will
		 * take care of setting the right page */

		g_signal_handlers_disconnect_by_func(adapter->proxy,
						property_changed, adapter);

		g_object_unref(adapter->proxy);
		g_free(adapter);
	}
}

static void
adapter_changed (GtkTreeModel *model,
		 GtkTreePath  *path,
		 GtkTreeIter  *iter,
		 GtkNotebook  *notebook)
{
	DBusGProxy *proxy;
	int i, count;
	gboolean is_default, powered;
	char *name;

	count = gtk_notebook_get_n_pages(notebook);

	gtk_tree_model_get(model, iter,
			   BLUETOOTH_COLUMN_PROXY, &proxy,
			   BLUETOOTH_COLUMN_DEFAULT, &is_default,
			   BLUETOOTH_COLUMN_POWERED, &powered,
			   BLUETOOTH_COLUMN_NAME, &name,
			   -1);

	for (i = 0; i < count; i++) {
		GtkWidget *widget;
		adapter_data *adapter;

		widget = gtk_notebook_get_nth_page(notebook, i);
		if (widget == NULL)
			continue;

		adapter = g_object_get_data(G_OBJECT(widget), "adapter");
		if (adapter == NULL)
			continue;

		adapter->is_default = is_default;

		if (proxy == NULL || adapter->proxy == NULL)
			continue;

		if (g_str_equal (dbus_g_proxy_get_path (proxy), dbus_g_proxy_get_path (adapter->proxy)) != FALSE) {
			if (is_default != FALSE && powered != FALSE)
				gtk_notebook_set_current_page (notebook, i);
			/* We usually get an adapter_added before the device
			 * is powered, so we set the name here instead */
			if (name)
				gtk_entry_set_text(GTK_ENTRY(adapter->entry), name);
		}
	}

	g_object_unref (proxy);
	g_free (name);
}

static void
button_clicked_cb (GtkButton *button, gpointer user_data)
{
	gtk_widget_set_sensitive (GTK_WIDGET (user_data), FALSE);
	bluetooth_killswitch_set_state (killswitch, KILLSWITCH_STATE_UNBLOCKED);
}

static gboolean
set_sensitive_now (gpointer user_data)
{
	gtk_widget_set_sensitive (GTK_WIDGET (user_data), TRUE);
	return FALSE;
}

static void
killswitch_state_changed (BluetoothKillswitch *killswitch,
			  KillswitchState state,
			  gpointer user_data)
{
	if (state != KILLSWITCH_STATE_UNBLOCKED)
		g_timeout_add_seconds (3, set_sensitive_now, user_data);
}

static void
create_killswitch_page (GtkNotebook *notebook)
{
	GtkWidget *mainbox;
	GtkWidget *vbox;
	GtkWidget *label;
	GtkWidget *button;

	GtkBuilder *xml;

	xml = gtk_builder_new ();
	if (gtk_builder_add_from_file (xml, "properties-adapter-off.ui", NULL) == 0) {
		if (gtk_builder_add_from_file (xml, PKGDATADIR "/properties-adapter-off.ui", NULL) == 0) {
			g_warning ("Failed to load properties-adapter-off.ui");
			return;
		}
	}

	vbox = GTK_WIDGET (gtk_builder_get_object (xml, "table1"));

	mainbox = gtk_vbox_new(FALSE, 24);
	gtk_container_set_border_width(GTK_CONTAINER(mainbox), 12);

	label = create_label(_("Bluetooth is disabled"));
	gtk_box_pack_start(GTK_BOX(mainbox), label, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(mainbox), vbox, TRUE, TRUE, 0);

	button = GTK_WIDGET (gtk_builder_get_object (xml, "button1"));
	g_signal_connect (button, "clicked",
			  G_CALLBACK (button_clicked_cb), button);
	g_signal_connect (killswitch, "state-changed",
			  G_CALLBACK (killswitch_state_changed), button);

	gtk_widget_show_all (mainbox);

	gtk_notebook_append_page(notebook, mainbox, NULL);
}

static void
create_no_adapter_page (GtkNotebook *notebook)
{
	GtkWidget *mainbox;
	GtkWidget *vbox;
	GtkWidget *label;
	GtkBuilder *xml;

	xml = gtk_builder_new ();
	if (gtk_builder_add_from_file (xml, "properties-no-adapter.ui", NULL) == 0) {
		if (gtk_builder_add_from_file (xml, PKGDATADIR "/properties-no-adapter.ui", NULL) == 0) {
			g_warning ("Failed to load properties-no-adapter.ui");
			return;
		}
	}
	vbox = GTK_WIDGET (gtk_builder_get_object (xml, "table1"));

	mainbox = gtk_vbox_new(FALSE, 24);
	gtk_container_set_border_width(GTK_CONTAINER(mainbox), 12);

	label = create_label(_("No Bluetooth adapters present"));
	gtk_box_pack_start(GTK_BOX(mainbox), label, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(mainbox), vbox, TRUE, TRUE, 0);

	gtk_widget_show_all (mainbox);

	gtk_notebook_append_page(notebook, mainbox, NULL);
}

void setup_adapter(GtkNotebook *notebook)
{
	killswitch = bluetooth_killswitch_new ();

	/* Create our static pages first */
	create_killswitch_page (notebook);
	create_no_adapter_page (notebook);

	client = bluetooth_client_new();

	adapter_model = bluetooth_client_get_adapter_model(client);

	g_signal_connect_after(G_OBJECT(adapter_model), "row-inserted",
					G_CALLBACK(adapter_added), notebook);
	g_signal_connect_after(G_OBJECT(adapter_model), "row-deleted",
					G_CALLBACK(adapter_removed), notebook);
	g_signal_connect_after (G_OBJECT(adapter_model), "row-changed",
				G_CALLBACK (adapter_changed), notebook);

	gtk_tree_model_foreach(adapter_model, adapter_insert, notebook);

	set_current_page (notebook);
}

void cleanup_adapter(void)
{
	g_object_unref(adapter_model);

	g_object_unref(client);

	g_object_unref (killswitch);
}


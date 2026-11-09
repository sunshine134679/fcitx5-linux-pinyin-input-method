import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences, gettext as _} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

const SETTINGS_SCHEMA = 'org.gnome.shell.extensions.kimpanel';

class KimpanelPrefsWidget extends Adw.PreferencesPage {
    static {
        GObject.registerClass(this);
    }

    constructor(settings) {
        super();
        this._settings = settings;

        const miscGroup = new Adw.PreferencesGroup();
        this.add(miscGroup);

        let toggle = new Gtk.Switch({
            action_name: 'kimpanel.vertical-list',
            valign: Gtk.Align.CENTER,
        });
        let row = new Adw.ActionRow({
            title: _('Vertical List'),
            activatable_widget: toggle,
        });
        this._settings.bind('vertical', toggle, 'active', Gio.SettingsBindFlags.DEFAULT);
        row.add_suffix(toggle);
        miscGroup.add(row);

        let button = new Gtk.FontButton();
        row = new Adw.ActionRow({
            title: _('Font'),
            activatable_widget: button,
        });
        this._settings.bind('font', button, 'font', Gio.SettingsBindFlags.DEFAULT);
        row.add_suffix(button);
        miscGroup.add(row);

        for (const [key, title, values] of [
            ['panel-theme', '候选配色', ['跟随桌面', '浅色', '深色', '经典蓝']],
            ['candidate-spacing', '候选间距', ['紧凑', '标准', '宽松']],
        ]) {
            const choice = new Adw.ComboRow({
                title,
                model: Gtk.StringList.new(values),
            });
            this._settings.bind(key, choice, 'selected', Gio.SettingsBindFlags.DEFAULT);
            miscGroup.add(choice);
        }
    }
}


export default class KimpanelExtensionPreferences extends ExtensionPreferences {
    getPreferencesWidget() {
        return new KimpanelPrefsWidget(this.getSettings());
    }
}

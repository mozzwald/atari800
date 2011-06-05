/*
 * Preferences.java - Preference activity for the emulator
 *
 * Copyright (C) 2010 Kostas Nakos
 * Copyright (C) 2010 Atari800 development team (see DOC/CREDITS)
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari800; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

package name.nick.jubanka.atari800;

import java.io.File;

import android.preference.PreferenceActivity;
import android.os.Bundle;
import android.preference.Preference;
import android.util.Log;
import android.content.SharedPreferences;
import android.preference.Preference.OnPreferenceClickListener;
import android.net.Uri;
import android.content.Intent;
import android.app.AlertDialog;
import android.webkit.WebView;
import android.app.Dialog;
import android.content.DialogInterface;


public final class Preferences extends PreferenceActivity implements Preference.OnPreferenceChangeListener
{
	private static final String TAG = "Preferences";
	private static final String[] PREF_KEYS = { "up", "down", "left", "right", "fire",
												"actiona", "actionb", "actionc" };
	private static final int ACTIVITY_FSEL = 1;
	private static final int DLG_ABOUT = 1;
	private static final int DLG_RESET = 2;
	private SharedPreferences _sp;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		KeymapPreference kp;

		super.onCreate(savedInstanceState);

		addPreferencesFromResource(R.xml.preferences);
		_sp = getPreferenceManager().getSharedPreferences();

		for (String s: PREF_KEYS) {
			kp = (KeymapPreference) findPreference(s);
			kp.setOnPreferenceChangeListener(this);
			kp.updateSum();
		}

		findPreference("rompath").setOnPreferenceClickListener(new OnPreferenceClickListener() {
			@Override
			public boolean onPreferenceClick(Preference p) {
				String val = _sp.getString("rompath", null);
				Uri u = (val == null) ? null : Uri.fromFile(new File(val));
				startActivityForResult(new Intent(FileSelector.ACTION_OPEN_PATH, u,
									   Preferences.this, FileSelector.class), ACTIVITY_FSEL);
				return true;
			}
		});

		findPreference("about").setOnPreferenceClickListener(new OnPreferenceClickListener() {
			@Override
			public boolean onPreferenceClick(Preference p) {
				showDialog(DLG_ABOUT);
				return true;
			}
		});

		findPreference("help").setOnPreferenceClickListener(new OnPreferenceClickListener() {
			@Override
			public boolean onPreferenceClick(Preference p) {
				startActivity(new Intent(Intent.ACTION_VIEW,
							  Uri.parse("http://pocketatari.atari.org/android/index.html#manual")));
				return true;
			}
		});

		findPreference("resetactions").setOnPreferenceClickListener(new OnPreferenceClickListener() {
			@Override
			public boolean onPreferenceClick(Preference p) {
				showDialog(DLG_RESET);
				return true;
			}
		});
	}

	@Override
	public boolean onPreferenceChange(Preference p, Object v) {
		int k = (Integer) v;
		KeymapPreference pref;

		Log.d(TAG, "Change " + k);
		for (String key: PREF_KEYS) {
			if (key.equals(p.getKey()))	continue;
			pref = (KeymapPreference) findPreference(key);
			if (k >= 0) {	// check mappings
				if (pref.getKeymap() == k)
					return false;
			} else {		// swap mappings
				if (pref.getKeymap() == -k) {
					pref.setKeymap( ((KeymapPreference) p).getKeymap() );
					return true;
				}
			}
		}
		return true;
	}

	@Override
	protected void onActivityResult(int reqc, int resc, Intent data) {
		switch (reqc) {
		case ACTIVITY_FSEL:
			if (resc != RESULT_OK) break;
			SharedPreferences.Editor e = _sp.edit();
			e.putString("rompath", data.getData().getPath());
			e.commit();
		}
	}

	@Override
	protected Dialog onCreateDialog(int id) {
		Dialog d;

		switch (id) {
		case DLG_ABOUT:
			WebView v = new WebView(this);
			v.loadData(String.format(getString(R.string.aboutmsg),
					   MainActivity._pkgversion, MainActivity._coreversion), "text/html", "utf-8");
			v.setVerticalScrollBarEnabled(true);
			d = new AlertDialog.Builder(this)
					.setTitle(R.string.about)
					.setIcon(R.drawable.icon)
					.setView(v)
					.setInverseBackgroundForced(true)
					.setPositiveButton(R.string.ok, null)
					.create();
			break;

		case DLG_RESET:
			d = new AlertDialog.Builder(this)
					.setTitle(R.string.warning)
					.setIcon(android.R.drawable.ic_dialog_alert)
					.setMessage(R.string.pref_warnresetactions)
					.setPositiveButton(R.string.ok, new DialogInterface.OnClickListener() {
						public void onClick(DialogInterface d, int i) {
							for (String str: new String[] {"actiona", "actionb", "actionc"})
								((KeymapPreference) findPreference(str)).setDefaultKeymap();
						}
						})
					.setNegativeButton(R.string.cancel, null)
					.create();
			break;

		default:
			d = null;
		}

		return d;
	}
}

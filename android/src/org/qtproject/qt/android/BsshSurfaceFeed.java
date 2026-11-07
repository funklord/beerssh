package org.qtproject.qt.android;

import android.view.Surface;
import android.view.View;
import android.view.ViewGroup;

/*
 * In Qt's package because QtWindow and its surface callback are package
 * private: a Qt that changes them then fails this build, where reflection
 * would have gone quiet on a phone. See BsshLifecycle.release() for why.
 */
public final class BsshSurfaceFeed {
	private BsshSurfaceFeed() {}

	/* Gives every Qt window under `view` this surface; says how many there were. */
	public static int feed(View view, Surface surface) {
		int windows = 0;
		if (view instanceof QtWindow) {
			((QtWindow) view).onSurfaceChanged(surface);
			windows++;
		}
		if (view instanceof ViewGroup) {
			final ViewGroup group = (ViewGroup) view;
			for (int i = 0; i < group.getChildCount(); i++)
				windows += feed(group.getChildAt(i), surface);
		}
		return windows;
	}
}

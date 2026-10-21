package org.rhythmtengoku.mobile;

import android.content.Context;
import android.widget.FrameLayout;

/** The surface fills the window; the Vulkan present path letterboxes the 16:9
 *  picture and draws the ambient border, so touch geometry can use the bars. */
final class GameViewport extends FrameLayout {
    GameViewport(Context context){super(context);}
}

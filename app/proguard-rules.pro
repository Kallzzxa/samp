-keep class com.nvidia.devtech.* { *; }
-keep class com.rockstargames.gtasa.* { *; }
-keep class com.wardrumstudios.utils.* { *; }

-keep class com.sampmobile.game.* { *; }
-keep class com.sampmobile.game.ui.* { *; }
-keep class com.sampmobile.game.ui.widgets.* { *; }
-keep class com.sampmobile.game.ui.widgets.adapter.* { *; }
# omp-cef browser layer: CefManager's class/method names are looked up by JNI directly by string
-keep class com.sampmobile.game.ui.cef.* { *; }

# for minify
-dontwarn javax.servlet.**
-dontwarn org.conscrypt.**
-dontwarn org.bouncycastle.**
-dontwarn org.openjsse.**
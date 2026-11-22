-keep class com.nvidia.devtech.* { *; }
-keep class com.rockstargames.gtasa.* { *; }
-keep class com.wardrumstudios.utils.* { *; }

-keep class com.sampmobile.game.* { *; }
-keep class com.sampmobile.game.ui.* { *; }
-keep class com.sampmobile.game.ui.widgets.* { *; }
-keep class com.sampmobile.game.ui.widgets.adapter.* { *; }

-keep class com.sampmobile.launcher.utils.SignatureChecker { *; }

# for minify
-dontwarn javax.servlet.**
-dontwarn org.conscrypt.**
-dontwarn org.bouncycastle.**
-dontwarn org.openjsse.**
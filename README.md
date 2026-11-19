# snap — أداة لالتقاط صور الشاشة بشكل مستمر ورفعها إلى Google Drive

أداة سطر أوامر خفيفة تعمل على Windows (`snap.exe`) وmacOS (`snap`) لالتقاط صور الشاشة بشكل مستمر للمحاضرات.

## ميزات نسخة Windows (`snap.exe`)
- **رفع مباشر من الذاكرة إلى Google Drive عبر OAuth 2.0** (دون حفظ الصور على القرص في الوضع الطبيعي).
- **فحص تغيير الشاشة بالمللي ثانية (بدون زمن محدد):** يلتقط لقطة واحدة في البداية، ثم يراقب الشاشة كل `100ms` بطريقة خفيفة جداً بدون ضغط على المعالج أو تعليق الجهاز، ولا يلتقط/يرفع إلا عند وجود حركة أو تغيير ولو بسيط (`0.01%`).
- **مجلد خاص على Google Drive:** ينشئ تلقائياً مجلداً باسم `Snap Lecture Captures` على Google Drive (أو الاسم المحدد عبر `--folder-name`) ويرفع جميع الصور داخله.
- **طابور ذكي عند انقطاع الإنترنت:** إذا انقطع الاتصال، تُحفظ الصور مؤقتاً في `pending_upload\` وتُرفع وتُحذف تلقائياً فور عودة الإنترنت.
- **العمل المستمر والتشغيل عند بدء الجهاز:** يدعم التشغيل التلقائي عند بدء Windows والعمل في الخلفية.

## التشغيل على Windows
1. **أول مرة:** شغّل `snap.exe` (أو `snap.exe --login`) ليفتح المتصفح لتسجيل الدخول بحساب Google، ثم يبدأ العمل تلقائياً.
2. **الإعداد التفاعلي (تحديد المدة، المجلد، والتشغيل عند بدء الجهاز):**
   ```cmd
   snap.exe --setup
   ```
3. **أهم الخيارات:**
   - `snap.exe` — يعمل باستمرار كل 5 ثوانٍ مع فحص تغير الشاشة والرفع إلى Drive.
   - `snap.exe -i 10 --diff 2.0` — كل 10 ثوانٍ وبنسبة تغير 2%.
   - `snap.exe --folder <DRIVE_FOLDER_ID>` — الرفع داخل مجلد محدد في Google Drive.
   - `snap.exe --install-startup -i 5 --background` — تفعيل التشغيل التلقائي عند بدء Windows في الخلفية.
   - `snap.exe --remove-startup` — إلغاء التشغيل التلقائي.
   - `snap.exe --local` — الحفظ محلياً فقط بدون Google Drive.
   - `snap.exe --logout` — حذف بيانات تسجيل الدخول المحفوظة.

## إعادة البناء
**Windows (MinGW):**
```bash
x86_64-w64-mingw32-g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++ windows_snap.cpp -o snap.exe -lgdiplus -lgdi32 -luser32 -ladvapi32 -lwinhttp -lws2_32 -lcrypt32 -lshell32 -lole32
```

**macOS:**
```bash
swiftc -O -swift-version 5 main.swift -o snap -framework Foundation
```

# TFM (TelegramFileManager) Integration for Mixxx

Esta guía explica cómo compilar Mixxx con la integración de TelegramFileManager y cómo usarlo.

## Tabla de Contenidos

1. [Requisitos Previos](#requisitos-previos)
2. [Compilación en Windows](#compilación-en-windows)
3. [Compilación en macOS](#compilación-en-macos)
4. [Compilación en Linux](#compilación-en-linux)
5. [Uso de TFM en Mixxx](#uso-de-tfm-en-mixxx)
6. [Configuración del Servidor TFM](#configuración-del-servidor-tfm)
7. [Solución de Problemas](#solución-de-problemas)

---

## Requisitos Previos

### Todos los sistemas operativos necesitan:
- **Git** - Para clonar el repositorio
- **CMake** 3.21 o superior
- **Python** 3.x (para scripts de build)
- Conexión a Internet (para descargar dependencias)

---

## Compilación en Windows

### Paso 1: Instalar Herramientas de Desarrollo

1. **Instalar Visual Studio 2022** (Community Edition es gratuito)
   - Descargar desde: https://visualstudio.microsoft.com/
   - Durante la instalación, seleccionar:
     - "Desarrollo para escritorio con C++"
     - SDK de Windows 10/11

2. **Instalar CMake**
   - Descargar desde: https://cmake.org/download/
   - Marcar "Add CMake to system PATH" durante la instalación

3. **Instalar Git**
   - Descargar desde: https://git-scm.com/download/win

### Paso 2: Clonar el Repositorio (si no lo has hecho)

```powershell
git clone https://github.com/mateof/TFM_mixxx.git D:\Repositorios\Net.Core\TFM_MIXXX
cd D:\Repositorios\Net.Core\TFM_MIXXX
```

### Paso 3: Descargar Dependencias Pre-compiladas

Mixxx proporciona dependencias pre-compiladas para Windows:

```powershell
# Crear carpeta para dependencias
mkdir buildenv
cd buildenv

# Descargar dependencias (usar la versión más reciente)
# Visitar: https://downloads.mixxx.org/dependencies/
# Descargar: mixxx-deps-2.4-x64-windows-release.zip

# Extraer en buildenv/
```

Alternativamente, usar vcpkg:

```powershell
# Instalar vcpkg
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

# Instalar dependencias de Mixxx
.\vcpkg install --triplet x64-windows `
    chromaprint fftw3 flac hidapi libebur128 libid3tag libkeyfinder `
    libmad libogg libopusenc libshout libsndfile libusb libvorbis `
    lilv mp3lame opus portaudio portmidi protobuf qt5-base qt5-svg `
    qt5-translations rubberband soundtouch sqlite3 taglib upower wavpack
```

### Paso 4: Compilar Mixxx

./mixxx.exe


Abrir "Developer PowerShell for VS 2022" o "x64 Native Tools Command Prompt":

```powershell
cd D:\Repositorios\Net.Core\TFM_MIXXX

# Crear directorio de build
mkdir build
cd build

# Configurar CMake
# Opción A: Con dependencias pre-compiladas de Mixxx
cmake .. -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_PREFIX_PATH="D:\Repositorios\Net.Core\TFM_MIXXX\buildenv"

# Opción B: Con vcpkg
cmake .. -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake"

# Compilar (Release para mejor rendimiento)
cmake --build . --config Release --parallel

# O abrir el proyecto en Visual Studio
start mixxx.sln
```

### Paso 5: Ejecutar Mixxx

```powershell
# Desde el directorio build
.\Release\mixxx.exe

# O en modo Debug
.\Debug\mixxx.exe
```

---

## Compilación en macOS

### Paso 1: Instalar Herramientas de Desarrollo

```bash
# Instalar Xcode Command Line Tools
xcode-select --install

# Instalar Homebrew (si no lo tienes)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Instalar dependencias
brew install cmake git python@3

# Instalar dependencias de Mixxx
brew install chromaprint fftw flac hidapi libebur128 libid3tag \
    libkeyfinder libmad libogg libopusenc libshout libsndfile \
    libusb libvorbis lilv mp3lame opus portaudio portmidi \
    protobuf qt@5 rubberband soundtouch sqlite taglib upower wavpack
```

### Paso 2: Clonar el Repositorio

```bash
git clone https://github.com/mateof/TFM_mixxx.git ~/Projects/TFM_MIXXX
cd ~/Projects/TFM_MIXXX
```

### Paso 3: Compilar Mixxx

```bash
# Crear directorio de build
mkdir build && cd build

# Configurar CMake
cmake .. \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qt@5)" \
    -DCMAKE_BUILD_TYPE=Release

# Compilar (usar todos los cores disponibles)
cmake --build . --parallel $(sysctl -n hw.ncpu)
```

### Paso 4: Ejecutar Mixxx

```bash
# Ejecutar directamente
./mixxx

# O crear un bundle de aplicación
cmake --build . --target package
# El .dmg estará en el directorio build/
```

---

## Compilación en Linux

### Ubuntu / Debian

#### Paso 1: Instalar Dependencias

```bash
# Actualizar sistema
sudo apt update && sudo apt upgrade -y

# Instalar herramientas de desarrollo
sudo apt install -y build-essential cmake git pkg-config

# Instalar dependencias de Mixxx
sudo apt install -y \
    libchromaprint-dev \
    libebur128-dev \
    libfaad-dev \
    libfftw3-dev \
    libflac-dev \
    libhidapi-dev \
    libid3tag0-dev \
    libkeyfinder-dev \
    liblilv-dev \
    libmad0-dev \
    libmodplug-dev \
    libmp3lame-dev \
    libogg-dev \
    libopus-dev \
    libopusfile-dev \
    libportaudio2 \
    libportmidi-dev \
    libprotobuf-dev \
    libqt5concurrent5 \
    libqt5opengl5-dev \
    libqt5sql5-sqlite \
    libqt5svg5-dev \
    libqt5x11extras5-dev \
    librubberband-dev \
    libshout3-dev \
    libsndfile1-dev \
    libsoundtouch-dev \
    libsqlite3-dev \
    libtag1-dev \
    libupower-glib-dev \
    libusb-1.0-0-dev \
    libvorbis-dev \
    libwavpack-dev \
    portaudio19-dev \
    protobuf-compiler \
    qt5-qmake \
    qtbase5-dev \
    qtbase5-dev-tools \
    qtscript5-dev \
    qttools5-dev \
    qttools5-dev-tools
```

#### Paso 2: Clonar y Compilar

```bash
# Clonar repositorio
git clone https://github.com/mateof/TFM_mixxx.git ~/TFM_MIXXX
cd ~/TFM_MIXXX

# Crear directorio de build
mkdir build && cd build

# Configurar CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# Compilar
cmake --build . --parallel $(nproc)
```

#### Paso 3: Ejecutar

```bash
# Ejecutar Mixxx
./mixxx

# Instalar en el sistema (opcional)
sudo cmake --install .
```

### Fedora / Red Hat

```bash
# Instalar dependencias
sudo dnf install -y \
    cmake gcc-c++ git \
    chromaprint-devel \
    fftw-devel \
    flac-devel \
    hidapi-devel \
    lame-devel \
    libebur128-devel \
    libid3tag-devel \
    libkeyfinder-devel \
    lilv-devel \
    libmad-devel \
    libmodplug-devel \
    libogg-devel \
    libsndfile-devel \
    libusb1-devel \
    libvorbis-devel \
    opus-devel \
    opusfile-devel \
    portaudio-devel \
    portmidi-devel \
    protobuf-devel \
    qt5-qtbase-devel \
    qt5-qtscript-devel \
    qt5-qtsvg-devel \
    qt5-qtx11extras-devel \
    rubberband-devel \
    libshout-devel \
    soundtouch-devel \
    sqlite-devel \
    taglib-devel \
    upower-devel \
    wavpack-devel

# Clonar y compilar
git clone https://github.com/mateof/TFM_mixxx.git ~/TFM_MIXXX
cd ~/TFM_MIXXX
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel $(nproc)
```

### Arch Linux

```bash
# Instalar dependencias
sudo pacman -S --needed \
    base-devel cmake git \
    chromaprint fftw flac hidapi lame libebur128 libid3tag \
    libkeyfinder lilv libmad libmodplug libogg libsndfile \
    libusb libvorbis opus opusfile portaudio portmidi \
    protobuf qt5-base qt5-script qt5-svg qt5-x11extras \
    rubberband libshout soundtouch sqlite taglib upower wavpack

# Clonar y compilar
git clone https://github.com/mateof/TFM_mixxx.git ~/TFM_MIXXX
cd ~/TFM_MIXXX
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel $(nproc)
```

---

## Uso de TFM en Mixxx

### Primera Configuración

1. **Iniciar Mixxx** con la integración TFM
2. **Ir a la barra lateral izquierda** (Library)
3. **Buscar "TelegramFileManager"** en la lista de bibliotecas externas
4. **Hacer click** en "TelegramFileManager"
5. **Aparecerá un diálogo** pidiendo la URL del servidor TFM
6. **Introducir la URL** de tu servidor TFM:
   - Ejemplo: `http://localhost:5000`
   - Ejemplo: `http://192.168.1.100:5000`

### Navegación

Una vez configurado, verás en la barra lateral:

```
TelegramFileManager
├── Channels (X)          <- Lista de canales de Telegram
│   ├── Canal 1 (150)     <- Nombre del canal (número de tracks)
│   ├── Canal 2 (200)
│   └── ...
├── Favorites             <- Canales marcados como favoritos
│   └── ...
└── Local TFM Folder      <- Carpeta local de TFM
    └── ...
```

### Cargar Tracks

1. **Click en un canal** para ver sus tracks
2. Los tracks aparecerán en la tabla principal
3. **Arrastrar un track** al Deck A o Deck B
4. El track se descargará/streameará automáticamente

### Opciones de Click Derecho

- **Add to Auto DJ** - Agregar al Auto DJ
- **Import as Mixxx Playlist** - Importar como playlist de Mixxx
- **Import as Mixxx Crate** - Importar como crate de Mixxx
- **Refresh** - Recargar la lista de canales
- **Configure TFM Server...** - Cambiar la URL del servidor

### Mostrar/Ocultar TFM

1. Ir a **Preferences** (Ctrl+P o Cmd+,)
2. Seleccionar **Library**
3. Buscar "Show TelegramFileManager Library"
4. Marcar/desmarcar según preferencia
5. **Reiniciar Mixxx** para aplicar cambios

---

## Configuración del Servidor TFM

### Requisitos del Servidor

El servidor TFM debe exponer estos endpoints:

| Endpoint | Método | Descripción |
|----------|--------|-------------|
| `/api/status` | GET | Estado del servidor |
| `/api/channels` | GET | Lista de canales |
| `/api/channels/{id}/tracks` | GET | Tracks de un canal |
| `/api/favorites` | GET | Canales favoritos |
| `/api/local/folders` | GET | Carpetas locales |
| `/api/local/folders/{id}/tracks` | GET | Tracks en carpeta local |
| `/api/tracks/{id}/stream` | GET | Stream de un track |
| `/api/tracks/{id}/download` | GET | Descargar track |
| `/api/search?q={query}` | GET | Buscar tracks |

### Formato de Respuesta Esperado

**Channels:**
```json
[
  {
    "id": "channel_123",
    "name": "Deep House Music",
    "description": "Best deep house tracks",
    "imageUrl": "http://server/images/channel_123.jpg",
    "trackCount": 150,
    "isFavorite": true
  }
]
```

**Tracks:**
```json
[
  {
    "id": "track_456",
    "channelId": "channel_123",
    "title": "Summer Vibes",
    "artist": "DJ Example",
    "album": "Summer Collection",
    "genre": "Deep House",
    "duration": 325,
    "fileUrl": "http://server/tracks/track_456.mp3",
    "localPath": "",
    "fileSize": 7864320,
    "coverUrl": "http://server/covers/track_456.jpg",
    "bpm": 124,
    "key": "Am"
  }
]
```

---

## Solución de Problemas

### Error: "TFM server URL is not configured"

**Solución:** Configura la URL del servidor haciendo click derecho en TelegramFileManager > Configure TFM Server...

### Error: "Network error" al conectar

**Posibles causas:**
- El servidor TFM no está corriendo
- URL incorrecta
- Firewall bloqueando la conexión
- El servidor no es accesible desde tu red

**Solución:**
1. Verificar que el servidor TFM está corriendo
2. Probar la URL en un navegador: `http://tu-servidor:puerto/api/status`
3. Verificar configuración de firewall

### Los tracks no se reproducen

**Posibles causas:**
- El formato de audio no es soportado
- El servidor no permite streaming
- Problemas de red durante la descarga

**Solución:**
1. Verificar que el track se puede descargar manualmente
2. Verificar los logs de Mixxx (Help > Debug Log)
3. Probar con un track diferente

### TelegramFileManager no aparece en la barra lateral

**Solución:**
1. Verificar que la compilación incluyó los archivos TFM
2. Ir a Preferences > Library
3. Asegurarse de que "Show TelegramFileManager Library" está marcado
4. Reiniciar Mixxx

### Error de compilación: "tfmfeature.h not found"

**Solución:**
1. Verificar que los archivos están en `src/library/tfm/`
2. Verificar que CMakeLists.txt incluye los archivos TFM
3. Limpiar y recompilar:
   ```bash
   cd build
   rm -rf *
   cmake ..
   cmake --build .
   ```

---

## Estructura de Archivos TFM

```
src/library/tfm/
├── tfmapiclient.h      # Cliente HTTP para la API de TFM
├── tfmapiclient.cpp
├── tfmfeature.h        # Feature principal (barra lateral)
├── tfmfeature.cpp
├── tfmtrackmodel.h     # Modelo de datos para tracks
└── tfmtrackmodel.cpp

res/images/library/
└── ic_library_tfm.svg  # Icono de TFM
```

---

## Contribuir

Si encuentras bugs o quieres agregar funcionalidades:

1. Fork el repositorio
2. Crea una rama para tu feature
3. Haz tus cambios
4. Envía un Pull Request

Repositorio: https://github.com/mateof/TFM_mixxx

---

## Licencia

Mixxx está licenciado bajo GPL v2. La integración TFM sigue la misma licencia.

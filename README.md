<div align="center">

# Grand Theft Auto III Zeebo

Port nativo de **Grand Theft Auto III (`re3`)** para **Zeebo (Qualcomm BREW 4.0.2)**

![Platform](https://img.shields.io/badge/Plataforma-Zeebo%20(BREW%204.0.2)-0066cc?style=flat-square)
![Arch](https://img.shields.io/badge/CPU-ARMv6K%20%2B%20VFPv2-1f6feb?style=flat-square)
![Graphics](https://img.shields.io/badge/GPU-OpenGL%20ES%201.1-2ea44f?style=flat-square)
![Release](https://img.shields.io/badge/Release-v1.0-d73a49?style=flat-square)

</div>

Port não oficial, gratuito e de código aberto desenvolvido por **Esther** ([pandoragg.eu.cc](https://pandoragg.eu.cc)), baseado no projeto [`re3`](https://github.com/GTAmodding/re3) e [`librw`](https://github.com/aap/librw). Sem vínculo oficial com a Rockstar Games. **Nenhum arquivo original do jogo é incluído neste repositório.**

> **Nota (`v1.0`):** Bugs gráficos e de desempenho são esperados nesta versão inicial.

---

## Recursos implementados

| Área | Detalhes |
| :--- | :--- |
| **Sistema (BREW 4.0.2)** | Ciclo `IApplet`, timer em 30 fps, syscalls de heap/arquivos via `pfnMalloc` e `IFileMgr`, streaming síncrono (`CdStreamZeebo`) e saves locais em `userfiles/` |
| **Gráficos (GLES 1.1)** | Backend `librw` em EGL `RGB565` + `DEPTH16` (`640x480`), texturas 16-bit (`565`/`5551`/`4444`) com mipmaps, vértices em `GL_FIXED`, skinning na CPU e iluminação `PRELIT` |
| **Desempenho** | Build `-O2` com hardware VFPv2 (`softfp`), cache de estado GLES1, batching de partículas e ajuste de LOD/draw distance para o Adreno 130 |
| **Vídeos (MPEG-1)** | Reprodução de `Logo.mpg` e `GTAtitles.mpg` via `pl_mpeg` direto em textura `RGB565` com skip por botão |
| **Áudio e Controles** | SFX de `SFX.RAW` e rádios/streams MP3/WAV via BREW `IMediaUtil`/`IMedia` (com atenuação 3D) e suporte nativo ao Z-Pad via `IHID` (dois analógicos) + `EVT_KEY` |

---

## Como preparar os assets

Você precisa dos arquivos de uma cópia legítima de **Grand Theft Auto III (PC)**. Extraia `gta3zeebo.mod` e `gta3zeebo.mif` da [Release v1.0](https://github.com/SterTheStar/gta3zeebo/releases/tag/v1.0) para uma pasta e copie para dentro dela as pastas originais do jogo:

- `anim/`, `audio/`, `data/`, `models/`, `movies/`, `TEXT/`, `txd/` (e `gamecontrollerdb.txt` se desejar).

Depois de montar a pasta (ex.: `dist/zeebo/gta3zeebo/`), basta carregá-la no **Zeebx** ou no console.

---

## Como compilar e rodar

Requisitos: **Arm GNU Toolchain** (`arm-none-eabi-gcc`), **CMake** (>= 3.16), **Ninja** e **Python 3**.

```powershell
cmake -S . -B build/zeebo -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/zeebo-arm.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/zeebo -j 8
```

*(O `CMakeLists.zeebo.txt` contém a configuração específica do alvo Zeebo.)*

O binário final será gerado em `build/zeebo/gta3zeebo.mod` e sincronizado automaticamente para `dist/zeebo/gta3zeebo/`.

---

## Controles (Z-Pad)

| Botão | Ação |
| :--- | :--- |
| **Analógico Esquerdo / D-Pad** | Movimentação, direção e navegação nos menus |
| **Analógico Direito** | Rotação da câmera |
| **B1 (`A`)** | Correr / acelerar / confirmar |
| **B2 (`B`)** | Atirar / frear / cancelar |
| **B3 (`X`)** | Pular / freio de mão |
| **B4 (`Y`)** | Entrar / sair do veículo |
| **ZL / ZR** | Trocar arma / mirar |
| **L2 / R2** | Trocar câmera / rádio / olhar para trás |
| **Start / Home** | Pausa / menu |

# Ray Regen Bridge

**Ray Reconstruction para placas AMD, via FSR Ray Regeneration**, num fork do [OptiScaler](https://github.com/optiscaler/OptiScaler).

> Nome antigo: `fsrd-cyberpunk`. No código e no `OptiScaler.ini` a feature continua chamada **FSRD** (classe `FSRDFeatureDx12`, seção `[FSRD]`).

Jogos como Cyberpunk 2077 só oferecem Ray Reconstruction (DLSS-RR) para placas NVIDIA. Este fork do OptiScaler intercepta esse pedido e o atende com o denoiser da AMD (FSR Ray Regeneration, `amd_fidelityfx_denoiser_dx12.dll`), seguido do upscale FSR que o OptiScaler já tinha.

> **Status:** experimental. O Ray Reconstruction liga no jogo e o denoiser roda sem travar; a qualidade ainda está sendo ajustada. Testado só no Cyberpunk 2077 (DX12), numa RX 9070 XT.

## Como funciona

Com o RR ligado, o jogo desliga os próprios denoisers e manda a imagem crua, com ruído. A cada frame, a classe `FSRDFeatureDx12` faz:

1. **Prepass** (`fsrd_prepass.hlsl`): separa a cor em sinal difuso e especular pelo albedo e converte profundidade, normais e motion vectors para o formato da AMD.
2. **Denoise**: FSR Ray Regeneration limpa os dois sinais, só em texturas próprias (nada do jogo é sobrescrito).
3. **Compose** (`fsrd_compose.hlsl`): multiplica de volta pelo albedo, soma os sinais e gera uma máscara reativa para os reflexos.
4. **Upscale**: o FSR do OptiScaler amplia a imagem limpa e escreve na saída do jogo.

Se alguma etapa falhar, ele volta a só fazer o upscale da cor original. Depois de 3 falhas, o denoiser fica desligado até o jogo reiniciar.

## Arquivos principais

| Arquivo | O que faz |
| --- | --- |
| `OptiScaler/upscalers/fsrd/FSRDFeatureDx12.*` | A feature: prepass, denoise, compose e upscale |
| `OptiScaler/upscalers/fsrd/ffx_denoiser_compat.h` | API do denoiser compatível com o `ffx_api.h` antigo que o OptiScaler usa |
| `OptiScaler/shaders/fsrd/` | Shaders de prepass e compose (HLSL e headers pré-compilados) |
| `OptiScaler/upscalers/FeatureProvider_Dx12.cpp` | Cria o FSRD quando o jogo pede DLSS-RR numa placa sem DLSS |
| `OptiScaler/NVNGX_Parameter.cpp`, `inputs/NVNGX_DLSS_Dx12.cpp` | Fazem o jogo e o Streamline enxergarem o RR como disponível |

## Compilar e instalar

1. Visual Studio 2022+ com "Desenvolvimento para desktop com C++" (toolset v145 testado).
2. Clonar com submódulos: `git clone --recurse-submodules` e abrir `OptiScaler.sln`.
3. Compilar em **Release x64**.
4. Copiar o `OptiScaler.dll` gerado para a pasta do jogo (`Cyberpunk 2077\bin\x64`) com o nome `winmm.dll`, junto do `OptiScaler.ini` e de `amd_fidelityfx_denoiser_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` e `amd_fidelityfx_dx12.dll`.
5. No jogo: Path Tracing ou Ray Tracing ligado, DLSS como upscaler e Ray Reconstruction ligado.

## Configuração

Seção `[FSRD]` do `OptiScaler.ini`; a maioria também aparece no menu do OptiScaler dentro do jogo.

| Opção | Padrão | Para que serve |
| --- | --- | --- |
| `EnableDenoiser` | `true` | `false` pula o denoiser e só faz upscale |
| `SplitMode` | `2` | Como dividir a cor: 0 por albedo, 1 tudo especular, 2 pela roughness |
| `ReflectionReactive` | `0.7` | Força da máscara reativa nos reflexos (menos rastro) |
| `DiffuseHitDistance` | `10.0` | Distância fixa dos raios difusos, que o DLSS-RR não envia |
| `SpecHitDistanceScale` | `1.0` | Multiplicador da distância dos reflexos enviada pelo jogo |
| `DebugView` | `false` | Mostra a tela de depuração do denoiser no lugar da imagem |
| `StabilityBias`, `RadianceClipStdK`, `KernelRelaxation` | padrão da AMD | Ajuste fino do denoiser |

## Limitações

- O DLSS-RR recebe só a cor final; separar difuso e especular pelo albedo é uma aproximação, então o resultado não é igual ao da NVIDIA.
- O jogo não envia a distância dos raios difusos; é usado um valor fixo.
- Só DX12. Vulkan e DX11 continuam com o comportamento original do OptiScaler.

## Créditos e licença

Baseado no [OptiScaler](https://github.com/optiscaler/OptiScaler) (snapshot do commit `6b911548`); a documentação original está lá. Usa o [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK). Licença GPL-3.0, a mesma do OptiScaler (ver `LICENSE`).

Projeto independente, sem afiliação com AMD, NVIDIA ou CD PROJEKT RED. AMD, FSR e FidelityFX são marcas da Advanced Micro Devices, Inc.; NVIDIA e DLSS são marcas da NVIDIA Corporation; Cyberpunk 2077 é marca da CD PROJEKT S.A. Os nomes aparecem só para descrever compatibilidade.

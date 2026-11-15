# CutoWarping

**Warp por corte e colagem, sem esticar o áudio.**
Criado por **DogSide Productions** e **Bluethedoggo**.

É o warp manual automatizado: o plugin corta o áudio **um pouquinho antes de cada kick**, move cada pedaço para que o kick caia **exatamente na grade**, e emenda tudo com um crossfade curto (no silêncio antes do kick, nunca em cima dele). Como não existe time-stretch, não tem efeito de "chorus" nem fase embolada: cada pedaço é o áudio original, intacto.

## CutoWarping x AutoWarp

| | CutoWarping | AutoWarp |
|---|---|---|
| Técnica | corta e move os pedaços | estica o áudio (time-stretch) |
| Som | áudio original, sem artefato de stretch | pode ter leve "chorus" em sons sustentados |
| Melhor para | música já **perto do tempo** do projeto (corrigir deriva) | mudar bastante o BPM |
| Velocidade | instantâneo | alguns segundos em músicas longas |

Os dois plugins podem ficar instalados juntos.

## Como funciona

1. Você carrega uma música (ou loop). O plugin acha o BPM e as batidas (o tempo pode oscilar).
2. Cada **corte** fica um pouco antes do kick (**PRE-ROLL**) e é encaixado num ponto sem clique (cruzamento por zero).
3. O pedaço entre dois cortes é movido para a grade:
   - **Pedaço longo demais** (o projeto é mais rápido): o final é aparado e some embaixo do próximo.
   - **Pedaço curto demais** (o projeto é mais lento): sobra um vão, preenchido repetindo o final do pedaço (**Gap fill: Repeat the end**) ou deixado em silêncio.
4. Entre dois cortes o áudio não é alterado. Quanto mais cortes, mais colado na grade.

## Controles

- **Cut every:** corta a cada batida, 2 batidas, compasso, 2 ou 4 compassos. Compasso a compasso mantém mais do áudio original; batida a batida deixa tudo mais preciso.
- **PRE-ROLL (ms):** quanto antes do kick o corte é feito (padrão 10 ms).
- **CROSSFADE (ms):** tamanho da emenda (padrão 5 ms; nunca passa do pré-roll).
- **Gap fill:** *Repeat the end* ou *Silence*.
- **QUANTIZE (%):** o quanto os cortes são puxados para a grade (100 % = totalmente na grade).
- **Quantize grid:** grade usada para os cortes que você adicionar à mão.
- **DETECT:** sensibilidade da detecção em arquivos curtos (loops).
- **Loop length (beats), /2, x2:** tamanho do loop em batidas (até 4096).
- **Playback:** *Host Sync* (travado na linha do tempo do FL) ou *MIDI Trigger* (cada nota dispara o loop).
- **PREVIEW**, **EXPORT WAV**, **LOAD** (ou arraste o áudio), **CLEAR CUTS**, **FIT**.

### Editar os cortes à mão

| Ação | Como |
|---|---|
| Adicionar corte | duplo clique na faixa de cima |
| Mover corte | arrastar o marcador de cima |
| Apagar corte | clique direito (ou Alt+clique) no marcador |
| Colocar um hit em outro lugar | arrastar o marcador de **baixo** (Shift = sem snap) |
| Voltar o marcador ao automático | clique direito no marcador de baixo |
| Início / fim do loop | arrastar as linhas verdes START / END |
| Zoom / rolagem | roda do mouse / Shift + roda |

## Instalar no FL Studio

> **IMPORTANTE:** a pasta **`CutoWarping.vst3`** precisa ficar em
> **`C:\Program Files\Common Files\VST3\`**

1. Descompacte o zip baixado.
2. Copie a pasta **`CutoWarping.vst3`** inteira (sem pastas por fora) para `C:\Program Files\Common Files\VST3\`.
   Resultado final: `C:\Program Files\Common Files\VST3\CutoWarping.vst3\Contents\x86_64-win\CutoWarping.vst3`
3. Botão direito em `CutoWarping.vst3\Contents\x86_64-win\CutoWarping.vst3` → **Propriedades** → se aparecer **Desbloquear**, marque e OK.
4. FL Studio → **Options → Manage plugins**, marque **Verify plugins** e clique em **Find installed plugins**.
5. Procure **CutoWarping** em **Plugin database → Installed → Generators**.

## Como gerar o `.vst3` (sem instalar nada)

1. Crie um repositório novo no GitHub e envie o conteúdo desta pasta (inclusive a pasta oculta `.github`). **Não envie a pasta `build`.**
2. Aba **Actions** → *Build CutoWarping* → aguarde uns 10 a 15 minutos.
3. Baixe o artefato **CutoWarping-Windows-VST3**. O `.exe` "Standalone" que vem junto é opcional e pode ser apagado (o Windows Defender às vezes o acusa por engano, por não ter assinatura digital).

## Limites

- Só vale a pena quando o tempo da música está **perto** do tempo do projeto. Se a diferença for grande, os vãos e cortes ficam longos demais; use o AutoWarp.
- Dentro de cada pedaço a música continua com o ritmo original. Se ela acelera ou desacelera muito entre dois cortes, use "Cut every: Every beat".
- No FL Studio, a opção **Host Sync** acompanha o BPM do projeto: o loop é recalculado se você mudar o BPM.

## Estrutura

```
Source/WarpEngine.*      motor (batidas, plano de cortes, corte e colagem), sem JUCE
Source/PluginProcessor.* parâmetros, thread de render, reprodução, estado do projeto
Source/PluginEditor.*    janela principal
Source/WarpView.*        forma de onda + cortes editáveis
tests/cut_test.cpp       teste do corte e colagem (g++ -O2 -std=c++17 -I../Source -I. cut_test.cpp ../Source/WarpEngine.cpp)
tests/song_test.cpp      teste do rastreador de batidas
tests/engine_test.cpp    teste do detector de transientes
```

## Créditos e licença
Criado por **DogSide Productions** e **Bluethedoggo** (aparece na janela, no ABOUT e nos metadados do plugin).
Veja `LICENSE.txt`: o plugin usa o framework JUCE (AGPLv3 ou licença comercial); se for distribuir o binário, respeite a licença do JUCE.

# Xfoil 0.2.0

Abra Xfoil pelo Homebrew Menu. X edita o endereco, R alterna
entre quatro servidores, Y carrega o catalogo, L busca e A abre detalhes.
Na tela de detalhes, A confirma o download. B cancela preservando o parcial.
Para retomar, selecione o mesmo arquivo. O servidor deve aceitar HTTP Range.
Os arquivos conferidos ficam em switch/xfoil/downloads/.

Para testar seu servidor, hospede catalogo-exemplo.json e arquivo-teste.txt
em HTTPS. No JSON, altere example.com para o endereco do arquivo hospedado
e cadastre a URL do JSON no aplicativo. O catalogo tem um array files.
Cada objeto exige title, file, url, size em bytes e sha256. Os campos
game e game_requirement sao opcionais. Cada arquivo pode ter ate 4 GiB
menos um byte. O catalogo aceita ate 500 entradas em 1 MiB.

Esta versao baixa arquivos e apresenta os requisitos do catalogo. A
instalacao de titulos e a compatibilidade com feeds proprietarios do
Tinfoil ainda nao foram implementadas.

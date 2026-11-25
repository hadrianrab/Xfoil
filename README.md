# Xfoil 0.3.0 — teste para Nintendo Switch

Extraia a pasta switch do ZIP na raiz do microSD e abra Xfoil pelo
Homebrew Menu. X edita o endereço do servidor, R alterna entre quatro
servidores, Y carrega o catálogo, L busca e A abre os detalhes.

Na tela de detalhes, A baixa e confere tamanho e SHA-256. B cancela a
transferência e guarda o parcial. O mesmo item retoma quando o servidor
aceita HTTP Range. Os arquivos completos ficam em switch/xfoil/downloads.

Em um NSP ou XCI conferido, A abre o DBI se ele estiver no cartão e o
Homebrew Menu aceitar o encaminhamento. No DBI, use Browse SD Card e
navegue até switch/xfoil/downloads para selecionar e instalar o arquivo.
O DBI continua responsável pela seleção e pela confirmação da instalação.

ZR, na lista, ativa a expiração em 8 horas. Ela vem desligada. O prazo
conta do fim do download de novos NSP/XCI e a limpeza ocorre ao reabrir
o Xfoil com a opção ativa. Downloads antigos, parciais e outros formatos
ficam fora da limpeza. A expiração do arquivo não desinstala o título.
Desative antes de reabrir ou mova um arquivo que queira guardar.

Cadastre um catálogo HTTPS com um array files. Cada objeto usa title,
file, url, size em bytes e sha256. game e game_requirement são opcionais.
O limite de arquivo é 4 GiB menos 1 byte.

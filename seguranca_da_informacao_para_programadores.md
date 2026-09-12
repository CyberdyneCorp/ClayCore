# Segurança da Informação para Programadores — C++, Python e SQL

> Material de estudo e base para uma apresentação extensa. O objetivo é formar
> programadores que projetam, implementam, revisam e operam software de forma
> segura — e não apenas decoram nomes de ataques.

## 1. Mentalidade: segurança é uma propriedade do sistema

Segurança da informação não é um produto, uma biblioteca ou uma etapa no fim
do projeto. É a redução contínua de riscos para pessoas, dados e operação.
Todo sistema tem ativos (dados, credenciais, dinheiro, reputação, serviço),
ameaças e vulnerabilidades. Uma vulnerabilidade só se torna incidente quando
alguém ou algo a explora em um contexto relevante.

**Risco ≈ probabilidade × impacto.** Uma senha exposta, por exemplo, pode ter
probabilidade alta de abuso e impacto enorme. Priorize corrigir riscos com
maior combinação de exposição e dano, em vez de perseguir apenas a lista mais
longa de falhas.

### A tríade CIA

- **Confidencialidade:** somente pessoas e serviços autorizados veem dados.
- **Integridade:** dados e ações não são alterados indevidamente.
- **Disponibilidade:** sistemas e dados estão acessíveis quando necessários.

Ela é complementada por autenticidade, rastreabilidade, privacidade e não
repúdio. Uma API que responde rápido mas revela perfis privados falha em
confidencialidade; uma tabela alterada por SQL injection falha em integridade;
um serviço derrubado por exaustão de recursos falha em disponibilidade.

## 2. SGSI, ISO 27001 e PDCA

Um Sistema de Gestão de Segurança da Informação (SGSI) organiza política,
pessoas, processos, tecnologia e evidências. A ISO/IEC 27001 usa o ciclo:

1. **Plan (planejar):** contexto, ativos, requisitos do negócio, critérios de
   aceitação de risco, ameaças, vulnerabilidades e tratamento.
2. **Do (fazer):** implementar controles, treinamento e procedimentos.
3. **Check (checar):** medir, auditar, testar e analisar incidentes.
4. **Act (agir):** corrigir causas, melhorar controles e atualizar riscos.

Escolher previamente tecnologias de hardware e software não substitui o
planejamento de risco: a arquitetura deve responder aos requisitos e riscos
identificados, não o contrário.

## 3. Modelagem de ameaças antes do código

Faça uma sessão curta antes de implementar cada funcionalidade relevante.

1. Desenhe fluxo de dados: navegador, API, banco, filas, armazenamento,
   serviços externos e administradores.
2. Liste ativos e fronteiras de confiança.
3. Pergunte: quem pode enviar entradas? quem pode ler, modificar ou apagar?
4. Use STRIDE: spoofing, tampering, repudiation, information disclosure,
   denial of service e elevation of privilege.
5. Defina controles e testes que provem esses controles.

Exemplo: em um endpoint `GET /users/{id}`, a entrada `id` pode ser válida,
mas a falha pode ser **IDOR/BOLA**: um usuário autenticado troca o id e lê o
perfil de outro. Validação de formato não resolve autorização por objeto.

## 4. Autenticação, autorização e sessão

**Autenticação** responde “quem é você?”. **Autorização** responde “você pode
fazer isto neste recurso agora?”. Nunca as trate como a mesma verificação.

- Armazene senhas com Argon2id, bcrypt ou scrypt; jamais SHA-256 puro.
- Use salt único e custo adequado; proteja também recuperação de senha.
- Prefira MFA para contas administrativas e operações sensíveis.
- Aplique menor privilégio e revisão periódica de permissões.
- Cookies de sessão devem usar `Secure`, `HttpOnly` e `SameSite`; expire e
  invalide sessões após troca de senha ou logout.
- Mensagens de login não devem revelar se um e-mail existe.

Exemplo Python com comparação em tempo constante para um token:

```python
import hmac

def token_valido(recebido: str, esperado: str) -> bool:
    return hmac.compare_digest(recebido.encode(), esperado.encode())
```

Não compare segredos com `==` quando a exposição a ataques de timing for
plausível. Mais importante: não grave tokens em logs, URLs ou mensagens.

## 5. Validação, normalização e codificação de saída

Todo dado externo é não confiável: formulário, arquivo, cabeçalho HTTP,
mensagem de fila, variável de ambiente, banco legado e outro microserviço.

- Valide por **allowlist**: tipo, tamanho, faixa e formato esperado.
- Normalize uma vez (Unicode, caminhos, encoding) antes de validar.
- Rejeite entradas ambíguas; limite tamanho e taxa de requisições.
- Codifique para o contexto de saída: HTML, atributo HTML, JavaScript, URL,
  comando, CSV e SQL exigem proteções diferentes.

Validação não “limpa” qualquer texto para qualquer uso. Um nome pode aceitar
acentos; um identificador de arquivo pode ter regras muito mais restritivas.

## 6. SQL injection: causa, impacto e defesa

SQL injection ocorre quando dados do usuário são concatenados à sintaxe SQL.
Pode vazar dados, burlar login, alterar ou apagar tabelas.

**Vulnerável (Python):**

```python
# Nunca faça isso: o conteúdo de email altera o comando SQL.
sql = "SELECT id FROM users WHERE email = '" + email + "'"
cursor.execute(sql)
```

**Correto (DB-API):**

```python
cursor.execute("SELECT id FROM users WHERE email = %s", (email,))
usuario = cursor.fetchone()
```

O placeholder é enviado separadamente do comando; ele não deve ser posto entre
aspas manualmente. Cada driver tem sintaxe própria (`%s`, `?`, `:name`).

Em C++, use a API parametrizada do driver/ORM. Exemplo conceitual:

```cpp
auto stmt = connection.prepare("SELECT id FROM users WHERE email = ?");
stmt.bind(1, email);
auto rows = stmt.execute_query();
```

Parâmetros normalmente não substituem nomes de tabela, coluna ou `ORDER BY`.
Para isso, escolha apenas valores de uma allowlist controlada pelo programa:

```python
ordens = {"nome": "name", "criado": "created_at"}
campo = ordens.get(sort, "created_at")
cursor.execute(f"SELECT id, name FROM users ORDER BY {campo} LIMIT %s", (limite,))
```

Defesa em profundidade: usuário do banco com privilégios mínimos, contas de
leitura separadas das de escrita, backups testados, logs sem segredos e alertas
para consultas anômalas. Nunca considere um WAF substituto de consultas
parametrizadas.

## 7. Segurança em Python

### Desserialização insegura

`pickle.loads()` pode executar comportamento controlado pelo conteúdo. Não
desserialize `pickle` de origem não confiável. Para intercâmbio use JSON com
schema e validação de tipos, mantendo o processamento de dados separado de
ações perigosas.

### Execução de comandos

```python
# Errado: permite que uma string altere a interpretação do shell.
subprocess.run("convert " + arquivo, shell=True)

# Melhor: sem shell, argumentos separados e caminho previamente validado.
subprocess.run(["convert", arquivo, saida], check=True, shell=False)
```

Mesmo sem shell, valide o arquivo, limite recursos e use diretório controlado.
Não aceite `../` para escapar de uma pasta permitida:

```python
from pathlib import Path

BASE = Path("/srv/uploads").resolve()
def arquivo_seguro(nome: str) -> Path:
    destino = (BASE / nome).resolve()
    if BASE not in destino.parents:
        raise ValueError("caminho fora da área permitida")
    return destino
```

### Dependências e segredos

Use ambiente virtual, arquivo de lock, atualização planejada e auditoria de
dependências. Chaves devem vir de cofre/variável de ambiente provisionada, não
do repositório. Rotacione uma chave exposta e remova-a do histórico quando o
processo organizacional permitir — apagar o texto do arquivo não a invalida.

## 8. Segurança em C++

C++ oferece desempenho e controle, mas exige disciplina sobre memória,
propriedade e limites. Buffer overflow, use-after-free, double free e integer
overflow podem virar execução de código ou negação de serviço.

**Vulnerável:**

```cpp
char nome[32];
std::strcpy(nome, entrada.c_str()); // transborda se entrada for longa
```

**Mais seguro:**

```cpp
std::string nome = entrada;
if (nome.size() > 31) throw std::invalid_argument("nome muito longo");
```

Prefira `std::string`, `std::vector`, RAII, smart pointers e contêineres da
biblioteca padrão. Ainda assim, valide comprimentos antes de alocar ou copiar.
Ao converter números, trate erro e intervalo:

```cpp
std::size_t limite = 0;
try {
    limite = std::stoull(texto);
} catch (const std::exception&) {
    throw std::invalid_argument("limite inválido");
}
if (limite > 1000) throw std::invalid_argument("limite excessivo");
```

Compile testes com sanitizers quando possível:

```bash
clang++ -std=c++20 -fsanitize=address,undefined -fno-omit-frame-pointer app.cpp
```

Use warnings rigorosos, análise estática, fuzzing de parsers e atualização de
bibliotecas. Mitigações de plataforma (ASLR, DEP/NX, stack canaries) ajudam,
mas não corrigem a causa.

## 9. Criptografia aplicada sem armadilhas

Não implemente algoritmos criptográficos próprios. Use bibliotecas maduras,
APIs de alto nível e configurações atuais.

- Para senha: Argon2id/bcrypt/scrypt; não “criptografe” senhas reversivelmente.
- Para dados em repouso: AEAD, como AES-GCM ou ChaCha20-Poly1305.
- Para transporte: TLS atual, validação de certificado e hostname.
- Para integridade/autenticação de mensagem: HMAC ou assinatura digital.
- Para aleatoriedade: `secrets` em Python e CSPRNG do sistema em C++.

Um nonce/IV em AEAD deve respeitar a regra da biblioteca e, frequentemente,
ser único por chave. Guardar a chave junto ao dado pode anular a proteção;
use KMS, HSM ou cofre de segredos conforme o risco.

## 10. Web: XSS, CSRF, SSRF e cabeçalhos

**XSS** acontece quando dados não confiáveis são interpretados como código no
navegador. Templates com autoescape são preferíveis. Não use `innerHTML` com
texto de usuário; se HTML for requisito, sanitize com biblioteca robusta e
política explícita. Aplique CSP como defesa adicional.

**CSRF** explora o envio automático de cookies. Proteja ações mutáveis com
token anti-CSRF, `SameSite` e confirmação para operações críticas.

**SSRF** faz o servidor requisitar destinos fornecidos pelo usuário. Ao aceitar
URLs, permita somente esquemas e hosts conhecidos, bloqueie IPs privados e de
metadados, limite redirects e use rede de saída restrita.

Cabeçalhos úteis incluem `Content-Security-Policy`, `X-Content-Type-Options:
nosniff`, `Referrer-Policy` e HSTS após validar HTTPS. Cabeçalhos não substituem
correção de falhas na aplicação.

## 11. Redes, protocolos e ataques comuns

DNS converte nomes em endereços; URL não “vira DNS”. HTTP clássico não cifra
conteúdo; HTTPS é HTTP sobre TLS e o `S` significa secure, não speed. Clientes
web modernos podem usar TCP/TLS ou QUIC sobre UDP, dependendo do protocolo,
portanto evite regras simplistas ao diagnosticar tráfego.

- **Spoofing:** falsificação de identidade/endereço; mitigue com autenticação,
  validação e proteção de rede.
- **Engenharia social:** manipula pessoas; treinamento e processo de
  confirmação são controles essenciais.
- **DDoS:** sobrecarrega recursos; use rate limit, CDN, cache, fila, autoscaling
  consciente e proteção de borda. É um tipo de ataque, não um malware.
- **Backdoor:** acesso oculto; trate como comprometimento e remova, investigue
  origem, segredos e persistência.

## 12. Malware e engenharia reversa básica

Malware inclui vírus, worm, trojan/cavalo de Troia, ransomware, spyware e
adware. “Ransomware corporativo” é uma categoria de ransomware; DDoS descreve
um ataque. Não execute amostras suspeitas em máquina pessoal. Faça análise
apenas em ambiente autorizado e isolado, com logs e procedimentos de resposta.

Controles: patching, EDR/antivírus, menor privilégio, backup imutável testado,
segmentação, allowlisting e monitoração. O backup precisa ser restaurável: um
backup nunca testado é só uma esperança.

## 13. Logging, privacidade e observabilidade

Logs são ativos sensíveis. Registre eventos de autenticação, autorização,
mudança de privilégio, acesso administrativo e erro relevante, com correlação
e horário confiável. Não registre senha, token, cookie, documento completo,
cartão, segredo de API ou conteúdo desnecessário.

Em vez de `logger.info("login %s senha=%s", email, senha)`, registre um ID
pseudonimizado, resultado e motivo genérico. Defina retenção, acesso e descarte
para logs. Proteja integridade e centralize-os para investigação de incidentes.

## 14. Segurança do ciclo de desenvolvimento

1. Requisitos de segurança e privacidade.
2. Modelagem de ameaças e desenho de arquitetura.
3. Padrões de código seguro e revisão por pares.
4. SAST, análise de dependências, testes unitários de autorização e scanners.
5. DAST/pentest autorizado em ambiente adequado.
6. CI/CD com segredos protegidos, artefatos assinados e permissões mínimas.
7. Monitoramento, patching, resposta a incidentes e retrospectiva.

Nunca coloque credenciais em `git`, imagens de container ou logs de CI. Use
scanners de segredos e falhe a pipeline quando identificar material válido.

## 15. Testes de segurança que todo programador deve escrever

- usuário A não lê/edita/revela recurso do usuário B;
- entrada excessivamente longa é rejeitada sem travar o processo;
- consulta com aspas/metacaracteres não muda a semântica SQL;
- token expirado, revogado ou alterado é negado;
- upload com extensão dupla, MIME falso ou path traversal falha;
- erro interno não expõe stack trace, SQL, chave ou detalhe de infraestrutura;
- limitação de taxa é aplicada a login e endpoints caros;
- rollback e backup de banco são efetivamente restauráveis.

Teste casos positivos e negativos. Uma cobertura alta que só testa “caminhos
felizes” não prova segurança.

## 16. Resposta a incidentes

Prepare antes do incidente: responsáveis, contatos, inventário, playbooks,
telemetria e backups. Durante um incidente:

1. Identifique e classifique o impacto.
2. Contenha sem destruir evidências desnecessariamente.
3. Preserve logs, snapshots e linha do tempo.
4. Erradique a causa e rotacione credenciais possivelmente expostas.
5. Recupere com validação e monitore recorrência.
6. Faça retrospectiva sem culpa e crie ações verificáveis.

Avisos legais, privacidade e comunicação externa devem seguir o processo da
organização e as leis aplicáveis, como a LGPD no Brasil.

## 17. Checklist para revisão de pull request

- A funcionalidade tem autorização por ação e por objeto?
- Entrada é validada, limitada e saída codificada pelo contexto?
- SQL, shell e templates usam APIs parametrizadas/seguras?
- Segredos estão fora do código, logs e respostas HTTP?
- Erros são seguros e úteis apenas para quem precisa diagnosticar?
- Dependências, compilador e imagens estão atualizados?
- A alteração adiciona testes para abuso previsível?
- Operação tem métricas, logs adequados e plano de rollback?

## 18. Exercícios para estudo

1. Transforme uma consulta concatenada em consulta parametrizada e escreva um
   teste com uma entrada contendo aspas.
2. Implemente um endpoint de perfil e escreva testes que impeçam IDOR.
3. Faça uma modelagem STRIDE de upload de avatar.
4. Crie uma política de logs para uma API de pagamentos fictícia.
5. Em C++, construa um parser de arquivo com limite de tamanho e execute-o com
   AddressSanitizer; depois gere entradas aleatórias para testar robustez.
6. Explique por que TLS não resolve controle de autorização nem SQL injection.

## Referências para aprofundamento

- ISO/IEC 27001 e ISO/IEC 27002.
- OWASP Top 10, OWASP ASVS, Cheat Sheet Series e Web Security Testing Guide.
- NIST Secure Software Development Framework (SSDF).
- CERT C++ Coding Standard e C++ Core Guidelines.
- Documentação do driver de banco usado pelo projeto para consultas preparadas.

## Fechamento

O melhor hábito de segurança é substituir “isso parece funcionar” por “quais
são as fronteiras de confiança, como isso pode falhar e qual teste demonstra
que o controle funciona?”. Código seguro nasce de decisões pequenas,
repetidas e verificadas durante todo o ciclo de vida.

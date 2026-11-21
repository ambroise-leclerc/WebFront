# ADR-024 — TLS intégré

## Versions

| Version | Date | Statut | Auteur | Changement | Référence |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-11-21 | Proposée | Ambroise Leclerc | Fournisseur TLS 1.3 optionnel et règles de qualification. | [PR #635](https://github.com/ambroise-leclerc/WebFront/pull/635) |

Un fournisseur réseau optionnel termine TLS 1.3 avant de livrer le flux au serveur HTTP et au pont WebSocket.
Le cœur HTTP garde son fournisseur TCP. Un port TLS sert exclusivement HTTPS et WSS avec HTTP/1.1.
Le profil initial utilise X25519 (avec HelloRetryRequest), ChaCha20-Poly1305, SHA-256 et ECDSA P-256
avec nonce RFC 6979. AES-GCM reste conditionné à une implémentation à temps constant raisonnable.
La reprise par tickets ajouterait une politique de rotation, une durée de vie et un second chemin de handshake.
Le premier profil fait un handshake complet à chaque connexion. Le délai est de dix secondes. Le serveur
écarte jusqu'à 64 Kio de données anticipées refusées pendant la négociation.

L'application fournit une chaîne PEM et une clé P-256 via la configuration. Le rechargement explicite
publie une paire complète après validation. Les handshakes en cours conservent leur instantané.
Le navigateur valide la chaîne et le nom du serveur. TLS protège le transport, tandis que la politique
d'origine continue de décider de l'accès au pont. Le profil ne demande aucun certificat au client.
Le serveur sait que le transport est chiffré lorsqu'il termine TLS et marque alors le cookie de liaison `Secure`.
Les lectures de fichiers pour un client non associé restent soumises à une déclaration explicite (ADR-020).

Le composant demeure header-only, sous `include/webfront/tls/`. Ses primitives sont implémentées dans WebFront à partir des spécifications
[RFC 7748](https://www.rfc-editor.org/rfc/rfc7748), [RFC 8439](https://www.rfc-editor.org/rfc/rfc8439),
[SEC 1](https://www.secg.org/sec1-v2.pdf), [SEC 2](https://www.secg.org/sec2-v2.pdf) et
[RFC 6979](https://www.rfc-editor.org/rfc/rfc6979). La génération de secrets utilise l'aléa de l'OS. Les opérations
sur des secrets visent des accès mémoire et un contrôle de flux indépendants de ces secrets. La qualification
exige une vérification du code produit par les compilateurs et architectures supportés. Cet audit reste à faire. Les vecteurs des RFC, les refus de protocole,
le fuzzing et un test navigateur HTTPS/WSS font partie de la qualification. Les régressions de sécurité
peuvent être signalées en privé aux mainteneurs. La maintenance du composant inclut la revue des primitives,
le suivi des errata des spécifications et des mesures reproductibles de taille et de mémoire.

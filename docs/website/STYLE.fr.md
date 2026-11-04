# Conventions de rédaction et de notation

Cette convention s’applique au livre français, à sa version anglaise britannique et aux textes du site.

## Notation

- Encadrer d’accents graves les noms de fonctions, types, variables, commandes, options, valeurs littérales et chemins : `describe`, `std::future<R>`, `wf test`, `--allow`, `main.cpp`.
- Le rendu utilise une police à chasse fixe, une couleur sombre et un fond blanc légèrement bordé. La distinction reste visible sans perception de la couleur. Ne pas employer l’italique pour du code.
- Réserver l’italique, écrit entre astérisques simples, à l’introduction d’un terme dans sa définition. Les noms de langages et de produits restent en romain.
- Une ligne vide sépare deux paragraphes. Un groupe de lignes commençant par `- ` produit une liste. Employer cette forme pour une série de fonctions ou d’options dont chacune demande une explication.
- Les blocs de code contiennent les extraits à saisir. Le texte précédent indique le fichier, le contexte et les prérequis. Les commandes exécutées dans le terminal WebFront doivent être distinguées de celles du terminal système.
- Le générateur accepte seulement ces formes simples ; il échappe le HTML du contenu. Les descriptions des métadonnées sont débarrassées des marques de notation.

## Exposition

Présenter le problème, expliquer le mécanisme, puis donner l’exemple et en préciser les limites. Un paragraphe développe une idée. Un changement de sujet ou de niveau d’explication demande un nouveau paragraphe.

Employer des phrases complètes et des titres descriptifs. Éviter les slogans, les suites de fragments, les questions rhétoriques, les triades décoratives et les métaphores de preuve ou de boucle. Décrire les avantages par les opérations qu’ils évitent ou rendent possibles. Distinguer les mesures publiées des appréciations générales.

Définir les termes avant de les utiliser. Préférer « répertoire », « compilation », « aperçu » ou « prévisualisation », « fichier de verrouillage » lorsque le terme français convient. Conserver la graphie exacte des API et des commandes.

Les exemples exécutables, les contrats de durée de vie et les chemins pris en charge ne doivent pas être modifiés pour les besoins du style. Toute limite technique utile à la compréhension doit rester explicite.

## Sources

Le contenu et les exemples sont dans `content.mjs`. Le rendu de la notation est dans `prose.mjs`, son emploi dans `build.mjs` et la typographie dans `dist/assets/site.css`. La notice « Conventions de notation » est présente dans chaque chapitre et ouverte au début du livre.

## Présentation du projet

Présenter WebFront d’abord comme un framework de création d’applications et expliquer la continuité entre développement, vérification, maintenance et distribution. La possibilité d’enrichir un programme existant est une propriété complémentaire. Le choix d’un programme console comme exemple pédagogique ne doit pas limiter ce positionnement. SiliconForge est une application conçue autour de WebFront ; attribuer les appréciations de vitesse à leur auteur et ne pas les transformer en mesure comparative universelle.

En français, écrire « Opensource » et « Jeu de la vie ». Réserver le nom Conway aux textes anglais.

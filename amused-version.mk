AMUSED_RELEASE=No
AMUSED_VERSION_NUMBER=0.5

.if ${AMUSED_RELEASE} == Yes
AMUSED_VERSION=${AMUSED_VERSION_NUMBER}
.else
AMUSED_VERSION=${AMUSED_VERSION_NUMBER}-current
.endif

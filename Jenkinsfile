node {
  stage 'checkout'
  checkout scm

  stage 'build'
  sh 'make'
  
  stage 'test'
  sh 'make test_software'
 }
